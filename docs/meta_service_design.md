# MetaBlkService（Meta Service）设计与实现详解

MetaBlkService（代码里也叫 `meta_service`，日志 tag 是 `metablk`）是 HomeStore 里专门用来管理"各子系统超级块（superblock）"的通用元数据持久化服务。LogStore、IndexService、BlkAllocator、Volume 等各个子系统自己的 superblock/元数据，都不需要各自实现持久化逻辑，而是统一通过这个服务来 add/update/remove/recover。它本质上是一个运行在独立 vdev 上的、**支持变长 context、支持多租户（subsystem type）、支持崩溃一致性恢复**的小型 KV 持久化引擎。

相关文件：
- 接口定义：`src/include/homestore/meta_service.hpp`
- 磁盘结构定义：`src/lib/meta/meta_sb.hpp`
- 实现：`src/lib/meta/meta_blk_service.cpp`（约 1631 行）

---

## 1. 整体设计思路

### 1.1 为什么要有这个服务

HomeStore 内部有很多模块都需要持久化自己的少量元数据（如 LogStore 的 store 列表、IndexTable 的根节点信息、BlkAllocator 的 bitmap 位置等）。如果每个模块都各自管理一块 vdev、自己处理 CRC、崩溃恢复、变长数据的溢出存储，会有大量重复代码且容易出错。MetaBlkService 把这些公共能力抽出来做成一个**单一 vdev 上的通用元数据仓库**：

- 每个子系统用一个字符串 `meta_sub_type`（如 "Logstore", "BlkAlloc"）作为"类型名"进行注册；
- 注册时提供回调 `meta_blk_found_cb_t`（找到一条属于自己的记录时调用）和 `meta_blk_recover_comp_cb_t`（该类型记录全部投递完毕时调用）；
- 之后该子系统可以调用 `add_sub_sb` / `update_sub_sb` / `remove_sub_sb` 来持久化任意大小的二进制 context_data，并得到一个 `cookie`（其实就是内存中 `meta_blk*` 指针）用于后续更新/删除。

### 1.2 磁盘布局（On-disk Layout）

在 `meta_sb.hpp` 的注释中画得很清楚，整体是一条**单链表**结构：

```
|----------|      |------------------|      |------------------|     |-------------------|
| Meta SSB | ---> | 1st Subsystem MB | ---> | 2nd Subsystem MB | --> | Last Subsystem MB | -> null
|----------|      |------------------|      |------------------|     |-------------------|
```

- **Meta SSB**（Super Super Block，`meta_blk_sb`）：整个 meta 链表的头节点，自己独占一个 block，存放 `next_bid`（指向第一个 subsystem meta blk）。
- **Meta Blk**（`meta_blk`，由 `meta_blk_hdr` + context_data 组成）：是一条子系统记录。它是**双向链表**节点（有 `prev_bid`/`next_bid`），这样删除中间节点时可以 O(1) 定位前后节点进行链表摘除。
- 如果 context_data 太大，单个 block（`block_size() - META_BLK_HDR_MAX_SZ`）放不下，就会溢出到 **Overflow Blk 链（ovf chain）**：

```
<meta_blk> --ovf_bid--> [ovf_hdr 1] --next_bid--> [ovf_hdr 2] --> ... --> null
                 |                         |
            data_bid[]                data_bid[]
                 v                         v
           [data blk...]             [data blk...]
```

每个 `meta_blk_ovf_hdr` 自己独占一个 block 做"目录"，里面存一组 `data_bid[]`（纯数据块），这样做的好处是**数据块可以 DMA 对齐写入，不需要拷贝到 header 里**。

### 1.3 关键磁盘结构体（`meta_sb.hpp`）

```cpp
struct meta_blk_sb {              // SSB，固定一个 block
    uint32_t magic;                // META_BLK_SB_MAGIC
    uint32_t version;
    BlkId next_bid;
    BlkId bid;                     // 自身 bid（自校验用）
    uint8_t migrated;              // 升级/迁移标记
};

struct meta_blk_hdr_s {            // 每条子系统记录的头
    uint32_t magic;                 // META_BLK_MAGIC
    uint32_t version;
    uint32_t gen_cnt;                // 每次 update 自增，用于调试/校验
    crc32_t  crc;
    BlkId next_bid, prev_bid, ovf_bid, bid;
    uint64_t context_sz;             // 落盘后的大小（若压缩，是压缩后 round up 的大小）
    uint64_t compressed_sz;          // 压缩后实际大小（解压用）
    uint64_t src_context_sz;         // 压缩前原始大小
    char type[MAX_SUBSYS_TYPE_LEN];  // 子系统类型名（64 字节）
    uint8_t compressed;
};
// 整个 header 用 padding 补齐到固定 META_BLK_HDR_MAX_SZ(512) 字节，
// 其余空间就是 inline 的 context_data（紧跟在结构体后面，用指针运算访问，
// 而不是用变长数组，避免 C++ 里非法的 0 长数组）

struct meta_blk_ovf_hdr_s {        // overflow 目录头，同样补齐占一个 block
    uint32_t magic;                 // META_BLK_OVF_MAGIC
    uint32_t nbids;                  // 本 ovf 块管理多少个 data blk
    BlkId next_bid, bid;
    uint64_t context_sz;             // 本节点覆盖的数据长度
};
```

所有结构体都用 `#pragma pack(1)` + 静态 `static_assert` 校验大小，保证 header 占用固定、对齐到 `CONTEXT_DATA_OFFSET_ALIGNMENT(64)`，这样紧跟其后的 context_data 起始地址天然是 DMA 对齐的，可以直接做 O_DIRECT 写入而不用额外拷贝对齐。

### 1.4 内存侧索引结构（`meta_service.hpp`）

```cpp
meta_blk_map_t     m_meta_blks;     // bid(uint64) -> meta_blk* 的全量缓存
ovf_hdr_map_t      m_ovf_blk_hdrs;  // ovf_bid -> meta_blk_ovf_hdr* 全量缓存（仅缓存 header，不缓存 data）
client_info_map_t  m_sub_info;      // type -> MetaSubRegInfo（回调 + 该类型拥有的所有 bid 集合）
subtype_graph_t    m_dep_topo_graph;// 子系统间恢复依赖图（DAG）
unique_ptr<BlkId>  m_last_mblk_id;  // 链表尾指针，加新记录 O(1)
meta_blk_sb*       m_ssb;           // SSB 内存镜像
```

`MetaSubRegInfo` 里记录了 `do_crc`（是否校验 CRC）、`meta_bids`（该类型拥有的所有 bid，用 `std::set` 便于按序遍历/删除）、`has_deps`（是否被其他类型依赖）。

可以看出：**所有 meta_blk 和 ovf_hdr 的 header 都常驻内存**（相当于全量元数据索引都在内存里），只有真正的 context_data 本体不缓存，需要时现读盘（`read_sub_sb_internal`）。这是一个空间换时间的设计——因为元数据本身量不大，但访问/恢复需要频繁定位。

---

## 2. 生命周期管理

### 2.1 创建/打开 vdev

- `create_vdev(size, devType, num_chunks)`：首次建 HomeStore 时调用，创建一个名为 `"meta"` 的 vdev，分配策略 `varsize`（变长分配器，因为 context_data 大小不固定），chunk 选择策略 `ROUND_ROBIN`，多盘模式 `ALL_PDEV_STRIPED`（跨所有物理盘条带化）。vdev 私有 context 存一个空的 `meta_vdev_context`（只有一个 `first_blkid` 字段，代表 SSB 的位置）。
- `open_vdev(vinfo, load_existing)`：重启时调用，从 `vdev_info` 的私有数据里取出 `meta_vdev_context`，校验类型是 `META_VDEV`；若 `first_blkid` 无效，说明上次初始化中途崩溃，直接抛异常要求以 init 模式重试。

### 2.2 `start(bool need_format)`

```cpp
void MetaBlkService::start(bool need_format) {
    reset_self_recover();
    alloc_compress_buf(init_compress_memory_size());
    if (need_format) {
        format_ssb();                 // 全新建
    } else {
        load_ssb();                    // 从盘上读出 SSB
        scan_meta_blks();               // 扫描整条链表，重建内存索引
    }
    recover();                          // 按依赖顺序回调各子系统
}
```

两种路径：
1. **`format_ssb()`**：首次格式化。分配一个 block 作为 SSB，初始化 magic/version/migrated=false/next_bid=invalid，然后 `write_ssb()` 落盘。
2. **`load_ssb() + scan_meta_blks()`**：
   - `load_ssb()`：读取 `meta_vdev_context->first_blkid` 指向的 block，校验 magic/version/self-bid/next_bid 合法性；
   - `scan_and_load_meta_blks()`：这是**崩溃恢复中最核心的一步**，从 SSB 的 `next_bid` 开始顺着链表逐个读出所有 `meta_blk`，并且：
     - 把每个 mblk 加入 `m_meta_blks` 缓存，同时记录到对应 type 的 `meta_bids` 集合；
     - **自愈逻辑**：如果发现当前 mblk 的 `prev_bid` 与"遍历时记录的上一个节点"不一致，说明上次在 `remove_sub_sb` 执行到一半时崩溃（前驱的 next 指针已更新，但后继的 prev 指针还没来得及更新，或者反过来），这里会直接修正并重新落盘该 mblk，同时设置 `self_recover = true` 标记（供上层/测试判断发生过自愈）；
     - 用 `m_sb_vdev->commit_blk()` 把这些 block 标记为"已分配"（因为 blk allocator 的位图信息不在这个链上，需要重新 replay 一遍占用状态）；
     - 若 context_sz 超过单 block 容量（`meta_blk_context_sz()`），说明数据在 overflow 链上，于是顺着 `ovf_bid` 继续遍历每个 `meta_blk_ovf_hdr`，同样校验 magic/self-bid，并 commit 其 header block 和所有 `data_bid[]`；
     - 做了一个"升级兼容性修复"：如果读出来累加的 `read_sz` 跟 header 里记录的 `context_sz` 不一致，会按配置 `skip_header_size_check` 决定是否 release-assert，否则直接拿读到的真实大小纠正内存里的 header（用于修复历史版本遗留的脏数据）。

### 2.3 `stop()`

加 `m_shutdown_mtx`（与 recover 互斥），清空内存缓存（`cache_clear()` 会 free 掉所有 `meta_blk*` 和 `ovf_hdr*` 的 iobuf），清空 `m_sub_info`，释放 SSB 内存和压缩 buffer，reset `m_sb_vdev`。

---

## 3. 注册机制与恢复依赖图

```cpp
void register_handler(type, cb, comp_cb, do_crc=true, deps=nullopt);
```

- 允许子系统在 SSB scan 完成**之后**才注册（因为 scan 阶段已经把所有 bid 按 type 分好类放进 `m_sub_info[type].meta_bids` 了，注册时只是把回调补上）；这里有个有意思的细节断言：如果 `m_sub_info` 里已经有这个 type（说明是 scan 阶段自动创建的占位项），要求此时 `cb`/`comp_cb` 必须还是空，防止重复注册覆盖。
- **依赖关系**：`deps` 参数允许声明"我依赖哪些其他 subtype 先完成恢复"。实现上为每个被依赖的 `x`，在 `m_dep_topo_graph[x]` 里加一条边指向当前 `type`，相当于建立了一张"谁要在谁之后恢复"的 DAG。
- `deregister_handler`：从 `m_sub_info` 和依赖图中移除。

### 恢复时的顺序保证（`recover()`）

```cpp
void MetaBlkService::recover(bool do_comp_cb) {
    std::lock_guard<decltype(m_shutdown_mtx)> lg{m_shutdown_mtx};
    meta_subtype_vec_t ordered_subtypes;
    if (hs_utils::topological_sort(m_dep_topo_graph, ordered_subtypes))
        throw ...; // 检测到循环依赖直接抛异常，拒绝启动

    for (auto& subtype : ordered_subtypes) recover_meta_sub_type(do_comp_cb, subtype);

    // 剩下没有声明依赖关系的子系统，没有顺序要求
    for (auto& [type, info] : m_sub_info)
        if (!info.has_deps) recover_meta_sub_type(do_comp_cb, type);
}
```

即：先对依赖图做**拓扑排序**，按拓扑序依次恢复有依赖关系的子系统；没有依赖声明的子系统目前是串行遍历（代码里 TODO 标注未来可以并发化）。

`recover_meta_sub_type` 对该 type 下所有 `meta_bids` 调用 `recover_meta_block`，最后触发 `comp_cb(true)`。

`recover_meta_block` 的细节：
1. 用 `read_sub_sb_internal` 把完整 context_data 读出来（inline 或走 ovf 链）；
2. 若该 type 注册了 `do_crc`，计算 CRC32 跟 header 里的 `crc` 比对，不一致直接 **release assert 崩溃**（元数据损坏是致命错误，不能静默跳过）；
3. 若 header 标记 `compressed`，用 `sisl::Compress::decompress` 解压（校验解压后大小必须等于写入时记录的 `src_context_sz`），再把解压后的 buffer 投递给回调；否则直接投递原始 buffer；
4. 回调签名是 `cb(meta_blk* mblk, sisl::byte_view buf, size_t size)`——把原始 mblk 指针也传回去，方便子系统在后续 update/remove 时直接拿它当 cookie。

还有一类"只注册、不要回调"的场景（`read_sub_sb(type)`）：子系统已经知道自己的数据在哪、什么时候要，不走全量 recover 回调，而是主动按类型拉取（典型场景是延迟初始化的模块）。

---

## 4. 写路径：`add_sub_sb`

```
add_sub_sb(type, context_data, sz, cookie)
  -> alloc_meta_blk(bid)                 // 分配 1 个连续 block
  -> m_sub_info[type].meta_bids.insert(bid)
  -> init_meta_blk(bid, type, context_data, sz)
       -> 分配并初始化 mblk 的 header（magic/version/gen_cnt=0/type）
       -> 处理链表挂接：
            若 m_last_mblk_id 有效：mblk.prev = *last; 且更新"旧的最后一个节点"的 next = bid（内存先改）
            否则（这是第一条记录）：mblk.prev = ssb.bid；ssb.next_bid = bid
       -> mblk.next_bid = invalid   // 自己就是新的尾节点
       -> write_meta_blk_internal(mblk, context_data, sz)   // 真正写数据（含压缩/溢出判断，见下）
       -> 【写完数据之后】才去持久化前驱节点的变更：
            如果有前一个节点：write_meta_blk_to_disk(前一个节点)
            否则：write_ssb()
       -> *m_last_mblk_id = bid；加入 m_meta_blks 缓存
  -> cookie = (void*)mblk
```

**顺序为什么这样设计是崩溃一致性的关键**：新节点必须先完整落盘（包括它的 `ovf` 数据块、以及自身 header），然后才去修改"前驱指向它"的指针并落盘。这样如果在"新节点还没写完"时崩溃，前驱节点（或 SSB）的 `next_bid` 还没被改，扫盘时根本不会发现这个半成品节点，相当于该分配的 block 只是被"浪费"了（后续可以通过 commit_blk 的占用重建或者垃圾回收策略处理），不会破坏链表完整性。

### `write_meta_blk_internal`：压缩 + inline/overflow 判定

1. **压缩**（受动态配置 `metablk.compress_feature_on` 控制）：
   - 仅当 `sz >= min_compress_size()`（配置 `min_compress_size_mb`）才尝试压缩；
   - 计算压缩后最大可能大小 `sisl::Compress::max_compress_len(sz)`，若超过 `max_compress_memory_size()` 直接放弃压缩（计数器 `compress_backoff_memory_cnt`）；
   - 若压缩缓冲区（`m_compress_info`）不够大则重新分配；
   - 调用 `sisl::Compress::compress`，算出压缩比 `ratio_percent = compressed_size*100/sz`；如果超过配置的 `compress_ratio_limit`（压缩效果不好）就放弃，使用原始数据（计数器 `compress_backoff_ratio_cnt`）；否则标记 `compressed=1`，记录 `src_context_sz`（原始大小）、`context_sz`（压缩后向 align_size 取整的大小）、`compressed_sz`（压缩后精确大小，用于 decompress 时传入）。整个过程用 `MetablkMetrics` 里的 counter/histogram 统计压缩成功率和压缩比分布。
2. **inline vs overflow**：
   - 若最终待写数据大小 `data_sz <= meta_blk_context_sz()`（= `block_size() - META_BLK_HDR_MAX_SZ`），直接 `memcpy` 到 mblk 紧跟其后的 context_data 区域，`ovf_bid` 设为 invalid；
   - 否则调用 `write_meta_blk_ovf()` 把数据写到 overflow 链，`mblk.ovf_bid` 记录链头。
3. **CRC**：无论 inline 还是 overflow，都在 mblk header 里存一份对**完整未压缩/实际写入内容**算出的 CRC32（`crc32_ieee`），只有子系统注册时声明了 `do_crc` 才计算。
4. 最后 `write_meta_blk_to_disk(mblk)` 把 header+inline数据所在的那个 block 整体落盘。
5. 代码里散布着 `#ifdef _PRERELEASE` 的 **crash flip point**（如 `write_with_ovf_abort`、`write_sb_abort`），这是专门为崩溃一致性测试设计的注入点：测试框架可以用 `HomeStoreFlip` 在特定位置强制模拟崩溃，验证重启后 `scan_and_load_meta_blks` 的自愈逻辑是否正确。

### `write_meta_blk_ovf`：overflow 链的崩溃一致写入

这是整个文件里对崩溃一致性考虑最细的一段：

```cpp
void MetaBlkService::write_meta_blk_ovf(BlkId& out_obid, const uint8_t* context_data, uint64_t sz, type) {
    // 1. 先把数据按 block_size 向上取整，一次性分配出所有需要的 data block
    alloc_meta_blks(round_up(sz, block_size()), context_data_blkids);
    // 2. 再分配第一个 ovf header block
    alloc_meta_blk(out_obid);
    ...
    while (next_bid.is_valid()) {
        // 按 ovf_blk_max_num_data_blk() 个一组切分 data block，组装一个 ovf_hdr
        // 如果剩余的 data block 数超过一个 ovf 节点能放下的数量，就再分配一个 ovf header block 作为 next
        write_ovf_blk_to_disk(ovf_hdr, context_data, sz, offset_in_ctx, type);  // 当前这个 ovf 节点立刻落盘
        offset_in_ctx += ovf_hdr->h.context_sz;
    }
}
```

注释里明确写道：

> The ovf blk is written to disk in reverse order to survive crash-in-the-middle; the tail is written first, then the prev blk of tail, until the head ovf blk. If crash happens before the head ovf blk is written, those blocks will simply be free after reboot.

也就是说虽然代码是正向遍历构造链表（从 head 往 tail 分配 bid），但是**实际写盘的语义效果是"尾节点写完才算数"**——因为 `meta_blk.ovf_bid` 这个指向链头的指针，只有在整条 ovf 链全部写完之后，才会被赋到 mblk header 里并随 `write_meta_blk_to_disk` 落盘（见 `write_meta_blk_internal` 里 `mblk->hdr.h.ovf_bid = obid;` 发生在 `write_meta_blk_ovf` 返回之后）。所以即使中途崩溃，mblk 本身要么还没落盘（ovf_bid 还是旧值或 invalid），要么 ovf 链已经全部写完——不存在"mblk 指向一条半成品 ovf 链"的中间态。

`write_ovf_blk_to_disk` 内部还处理了 **DMA 对齐**问题：若传入的 `context_data + offset` 指针没有对齐，或者长度不是对齐的整数倍，会临时分配一块对齐的 iobuf，`memcpy` 过去并在尾部补零后再写，写完立即释放临时 buffer——这是因为上层 `VirtualDev::sync_write` 走的是 `O_DIRECT`，必须严格对齐。

---

## 5. 更新路径：`update_sub_sb`（原地更新语义）

```cpp
void update_sub_sb(context_data, sz, cookie) {
    加锁 m_meta_mtx
    mblk = (meta_blk*)cookie
    old_ovf_bid = mblk->ovf_bid                 // 先记下旧的溢出链，稍后释放
    mblk->compressed = 0
    mblk->ovf_bid.invalidate()
    mblk->gen_cnt += 1
    write_meta_blk_internal(mblk, context_data, sz)   // 复用写路径：重新判定压缩/inline/overflow，分配新的 ovf 链（如果需要），写盘
    free_ovf_blk_chain(old_ovf_bid)              // 新数据落盘完成后，才释放旧的溢出链
}
```

关键点：**mblk 这条记录本身的 bid 不变**（原地更新），只是 header 内容和 ovf 链会变化；而且顺序是"先写新数据，再释放旧的 overflow 空间"，保证任何时刻都至少有一份完整数据在磁盘上可恢复。`gen_cnt` 自增是用来做调试断言的（`_cookie_sanity_check` 比较缓存和盘上/传入 cookie 的 `gen_cnt` 是否一致）。

---

## 6. 删除路径：`remove_sub_sb`（双向链表摘除）

```
rm_blk = (meta_blk*)cookie
prev_bid = rm_blk.prev_bid; next_bid = rm_blk.next_bid

if prev_bid == ssb.bid:                 // 删除的是链表第一个节点
    ssb.next_bid = next_bid; write_ssb()
    if last_mblk == rm_bid: last_mblk.invalidate()
else:
    prev_node.next_bid = next_bid; write_meta_blk_to_disk(prev_node)

if next_bid 有效:
    next_node.prev_bid = prev_bid; write_meta_blk_to_disk(next_node)
else:                                    // 删除的是尾节点
    last_mblk = prev_bid

m_meta_blks.erase(rm_bid)
m_sub_info[type].meta_bids.erase(rm_bid)
free_meta_blk(rm_blk)                     // 释放磁盘 block + 可能的 ovf 链 + 内存
```

这正是前面提到的 `scan_and_load_meta_blks` 自愈逻辑要处理的场景：如果在"更新 prev 节点的 next 指针"和"更新 next 节点的 prev 指针"这两步之间崩溃，重启扫描时会发现某个节点的 `prev_bid` 字段和"实际遍历顺序中的前驱"对不上，从而自动修正。

`free_meta_blk` / `free_ovf_blk_chain` 释放磁盘空间都是调用 `m_sb_vdev->free_blk()`，并有 `HS_DBG_ASSERT_EQ` 断言释放前后 `used_size()` 的差值恰好等于释放的 block 数 × block_size，用来在 debug 模式下及时发现 allocator 记账错误。

---

## 7. 读路径

两种读取方式：
1. **`recover()` 过程中自动投递**（被动）：适用于启动时拉取全部历史数据；
2. **`read_sub_sb(type)`**（主动）：子系统任何时候都可以主动按 type 把自己名下所有 meta_bids 读出来逐条回调（要求此前已注册 `cb`）。

底层都走 `read_sub_sb_internal(mblk)`：
- 若 `context_sz <= meta_blk_context_sz()`：数据是 inline 的，直接从 `mblk->get_context_data()` memcpy 出来；
- 否则遍历 `ovf_bid` 链，对每个 ovf header 的 `data_bid[]` 逐个发起 `read()`（`m_sb_vdev->sync_read`），按 `align_size()` 取整，拼接成完整 buffer，并在循环里持续用 `HS_REL_ASSERT_EQ` 校验自身 bid、累计长度与 header 声明的 `context_sz` 是否吻合。

压缩数据的解压只发生在 `recover_meta_block`（因为目前约定走 `read_sub_sb` 读回的数据本身不会是压缩过的客户端——代码里有注释明确这个假设）。

---

## 8. 数据完整性与自检工具

- **`_cookie_sanity_check(cookie)`**：仅 `_PRERELEASE` 编译下调用，校验 cookie 的 magic/version、prev/self bid 有效性、该 type 已注册、以及这个 bid 确实存在于 `m_sub_info[type].meta_bids` 和 `m_meta_blks` 缓存中，且缓存里的内容（gen_cnt、next/prev）跟传入的一致——用于及早发现"客户端拿着野指针/过期 cookie 来调用"的 bug。
- **`ssb_sanity_check()`**：重新从盘上读一次 SSB，跟内存镜像比较 magic/version/self-bid。
- **`sanity_check(check_ovf_chain)`** / 对外暴露为 **`verify_metablk_store()`**：拿全局锁，从 SSB 开始重新完整遍历一遍磁盘上的链表（不依赖内存缓存！），逐节点跟内存缓存做 `_cookie_sanity_check`，校验 prev/next 链接的自洽性，统计磁盘上的节点数跟内存缓存记录数是否一致，以及磁盘上出现的 client 类型数不应超过注册的 client 数；`check_ovf_chain=true` 时还会把每条 overflow 链也完整读一遍校验 magic/self-bid/nbids。这是最权威的"离线一致性校验"工具，通常用在测试或诊断场景，因为代价是独占全局锁、全量扫盘。
- **`get_status(log_level)` / `populate_json()` / `dump_disk_metablks(client)`**：用于运维/调试导出诊断信息，`log_level` 越高导出信息越详细（1=汇总统计，2=列出所有 bid，3=连内容都 dump 到 `/tmp/dump_meta/` 文件，并且会检查磁盘剩余空间上限 `percent_of_free_space` 防止把 `/tmp` 写爆）。`dump_disk_metablks` 的特殊之处是它**不使用任何内存缓存，而是重新 `scan_and_load_meta_blks` 到局部变量里**，保证反映盘上真实状态而不是进程内存状态，调试双方状态不一致的问题时很有用。

---

## 9. 并发与锁模型

- **`m_meta_mtx`**：保护所有对 `m_meta_blks` / `m_ovf_blk_hdrs` / `m_sub_info` / SSB 的读写操作（add/update/remove/register/deregister/read_sub_sb/sanity_check 等都会持有它）。
- **`m_shutdown_mtx`**：专门协调 `recover()` 和 `stop()` 之间的并发——因为代码注释特别指出，**客户端的 recover 完成回调（`comp_cb`）内部，可能会反过来调用 `add_sub_sb`/`remove_sub_sb` 等 API**，这是一个合理的重入场景（比如某子系统在恢复完成后立刻要写一条新纪录），所以 `recover()` 本身不持有 `m_meta_mtx`（否则会自己把自己锁死），而是用一把更粗粒度的 `m_shutdown_mtx` 只是为了防止"正在 recover 的时候被 stop() 并发释放资源"。

---

## 10. 压缩/对齐等可调参数（HS_DYNAMIC_CONFIG）

通过动态配置暴露了一组可运行时调整的参数（均以 `metablk.*` 为前缀）：
- `compress_feature_on`：全局开关；
- `min_compress_size_mb`：小于此大小不压缩（避免浪费 CPU）；
- `max_compress_memory_size_mb` / `init_compress_memory_size_mb`：压缩缓冲区大小上限/初始大小；
- `compress_ratio_limit`：压缩比阈值，超过说明压缩效果不好，放弃使用压缩结果；
- `skip_header_size_check`：兼容性开关，允许跳过升级场景中 context_sz 对不上的 release assert；
- `percent_of_free_space`：dump 到文件时允许使用的最大磁盘空间比例。

另外 `MetablkMetrics`（继承 `sisl::MetricsGroupWrapper`）统计了压缩成功次数、因内存超限/比例超限回退的次数，以及压缩比的直方图，便于监控压缩特性是否按预期工作。

---

## 11. 整体设计亮点小结

1. **单链表 + 溢出链**的两级结构，用固定大小的 header block 承载索引信息，变长数据单独走对齐的数据块，兼顾了元数据定长可控与 context 数据任意大小的需求。
2. **"先写新/子节点，再修改父/前驱指针"的写入顺序**贯穿 add/update/remove 全流程，是实现崩溃一致性的核心手段，配合启动时的 `scan_and_load_meta_blks` 自愈逻辑（检测并修复 prev/next 不一致）完成"软件层面的日志式一致性"，而不依赖额外的 WAL。
3. **压缩是尽力而为、可回退的**：压缩比不好或资源超限都会自动回退到不压缩，不会因为压缩失败而写坏数据。
4. **子系统间恢复顺序通过显式依赖声明 + 拓扑排序**保证，检测到环直接拒绝启动，比隐式顺序假设更健壮。
5. **全部内存索引常驻 + 数据按需读盘**的设计，适合元数据量不大但种类多、访问模式稀疏的场景。
6. 提供了与业务逻辑解耦的**独立离线校验工具**（`sanity_check`/`dump_disk_metablks`），可以在不信任内存状态的前提下复核磁盘真实状态，这对分布式存储系统的可运维性非常关键。
7. 大量 `_PRERELEASE` 专属的 crash-flip 注入点和 CRC/assert 校验，说明这个模块把"崩溃恢复正确性"当作头等大事来做测试覆盖，而不仅仅是正常路径功能正确。
