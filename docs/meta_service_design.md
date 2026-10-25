# HomeStore Meta Service 设计与实现详解

`MetaBlkService` 是 HomeStore 的持久化控制面 metadata 服务：它为 CP、Index、LogStore、复制服务和 allocator 保存、更新、删除并在启动时恢复 superblock。它不是高吞吐 KV/WAL，而是支持多客户端、变长 payload、CRC、压缩、恢复回调和依赖有序恢复的 superblock 管理器。

> 主要代码：`src/include/homestore/meta_service.hpp`、`src/lib/meta/meta_sb.hpp`、`src/lib/meta/meta_blk_service.cpp`、`src/include/homestore/superblk_handler.hpp`。设计简介在 `src/lib/meta/README.md`。

## 架构与职责

```text
Client subsystem
  ├─ register_handler(type, found_cb, completion_cb)
  ├─ add_sub_sb(payload)       -> cookie
  ├─ update_sub_sb(payload, cookie)
  └─ remove_sub_sb(cookie)
             ↓
       MetaBlkService
             ↓
    META_VDEV / VirtualDev
             ↓
        Physical devices
```

HomeStore 先启动 Meta Service，再启动 CPManager、IndexService 等；这些上层服务会在构造期注册 metadata handler。启动顺序见 `src/lib/homestore.cpp:169-188`、`276-315`。

典型客户端 type：`CPSuperBlock`、`index`、`wb_cache`、`LogStoreServiceSB`、`Logdev_sb`、`Logdev_rollback_sb`、`repl_dev`。一个 type 可以有多个 metadata record，例如每张 IndexTable 都使用 `index` type、但拥有独立 record。

## 公共 API 和 callback

定义于 `meta_service.hpp:80-379`：

```cpp
register_handler(type, found_cb, completion_cb, do_crc = true, deps = nullopt)
deregister_handler(type)
add_sub_sb(type, data, size, cookie)
update_sub_sb(data, size, cookie)
remove_sub_sb(cookie)
read_sub_sb(type)
recover()
```

`add_sub_sb` 返回的 `cookie` 实际是内存中的 `meta_blk*`；它是后续 update/remove 的唯一稳定句柄。恢复时，系统对同一 type 的每一条记录调用 `found_cb(meta_blk*, payload, size)`，全部记录完成后调用 `completion_cb(true)`。

`do_crc` 默认为 true，恢复时会对存储 payload 做 CRC32 校验。任意 metadata type 的名称直接写入磁盘 header，故 type 名称是持久化协议的一部分，不能随意重命名。

## `superblk<T>` 包装器

上层一般不直接调 Meta Service，而使用 `superblk<T>`（`superblk_handler.hpp:26-132`）：

```text
首次 write()：add_sub_sb，保存返回 cookie
后续 write()：update_sub_sb，复用 cookie
destroy()：remove_sub_sb(cookie)
```

它还负责对齐 buffer、从恢复 `byte_view` load、resize、move 和 raw buffer 生命周期。`json_superblk` 则将 JSON 序列化为 MessagePack 后使用同一机制。

## META_VDEV

Meta Service 创建 `META_VDEV`，使用：

```text
optimal page size 作为 block size
varsize allocator
ROUND_ROBIN chunk selector
ALL_PDEV_STRIPED 多设备策略
```

见 `meta_blk_service.cpp:50-65`。VDev 的 user-private context 保存 `first_blkid`，即 Meta Super Super Block（SSB）位置。重启后 HomeStore 根据 `META_VDEV` context 将 VDev 路由回 MetaBlkService，见 `homestore.cpp:447-474`。

## 磁盘布局

```text
META_VDEV private context
  └─ first_blkid
       └─ Meta SSB
            └─ next_bid → MetaBlock 1 ↔ MetaBlock 2 ↔ ...
```

SSB (`meta_blk_sb`) 包含 magic、version、自身 bid、首 metadata block `next_bid` 和 migrated 标志，见 `meta_sb.hpp:97-111`。

每个普通 metadata record 使用固定 512B header：

```text
magic/version/gen_cnt/crc
type
self bid / prev bid / next bid
overflow bid
context size / compressed size / source size / compressed flag
inline context payload（若足够小）
```

定义见 `meta_sb.hpp:119-170`。`gen_cnt` 每次 update 增加；header 固定 512B 以稳定格式和 payload 偏移。

### 小 payload

```text
context_sz <= block_size - 512
```

payload 直接存放在普通 meta block header 后。

### 大 payload

普通 block 仅保存 `ovf_bid`，payload 位于 overflow chain：

```text
normal MetaBlock → overflow header → data blocks
                         │
                         └→ next overflow header → data blocks
```

overflow header 保存一批 data block ID、该段真实长度和 next header，见 `meta_sb.hpp:173-223`。这样可适应任意较大 metadata，并处理 DMA 对齐写入。

## 首次启动和重启

首次格式化：

```text
分配 SSB block
→ 写 VDev private context.first_blkid
→ 初始化 SSB（next_bid invalid）
→ 同步写 SSB
```

重启：

```text
读 first_blkid
→ load SSB 并校验 magic/version/self bid
→ 从 SSB.next_bid 遍历普通 metadata 链
→ 读取 overflow headers
→ 对所有已使用 block 调 allocator commit_blk
→ 重建 type → meta_bids、block cache、last block
→ 按依赖顺序恢复客户端
```

见 `meta_blk_service.cpp:88-105`、`144-163`、`219-354`。恢复时 commit 已占用的普通块、overflow header 块和 data 块，避免 allocator 将它们重复分配。

## Add 的崩溃一致性

新增记录的顺序（`init_meta_blk`，`meta_blk_service.cpp:544-600`）：

```text
初始化新 record 的 prev/next
→ 写新 record 和 payload/overflow chain
→ 更新并写旧 tail.next 或 SSB.next
→ 更新内存 last block 与 cache
```

即“**先持久化新对象，后发布链表引用**”。在链接发布之前崩溃，新 record 不可达，不会破坏既有 metadata 链。

大 payload 的 overflow chain 先从尾到头写入；全部 overflow data/header 成功后，主 metadata block 才发布 `ovf_bid`。因此主 record 不会指向半成品 overflow chain，见 `meta_blk_service.cpp:603-666`。

## Update 的崩溃一致性

更新复用同一主 `meta_blk`：

```text
保存旧 ovf_bid
→ 清空 compressed / ovf_bid，gen_cnt++
→ 写新 payload（必要时新建 overflow chain）
→ 写主 meta block，使其切换到新 ovf_bid
→ 释放旧 overflow chain
```

见 `meta_blk_service.cpp:799-870`。

这是一种 Copy-then-Switch：主 block 写前崩溃仍保留旧引用；主 block 写后崩溃则新内容已可恢复，旧 overflow chain 最多暂时泄漏空间，正确性优先。

## Remove 与链表自修复

删除会更新：

```text
prev.next = next
next.prev = prev（若 next 存在）
→ 移除缓存和 type 的 bid 索引
→ 释放主 block 和 overflow chain
```

见 `meta_blk_service.cpp:872-950`。

若在两侧链表指针之间崩溃，正向 `next` 链仍可从 SSB 遍历。扫描时以遍历到的前驱为权威；发现当前 block 的 `prev_bid` 不匹配，就修正并同步写回。见 `meta_blk_service.cpp:250-257`。这使 remove 中断能自动恢复双向链一致性。

## Payload 读取、CRC 与压缩

恢复时：

```text
read_sub_sb_internal
→ 从 inline 或 overflow chain 读取实际磁盘 payload
→ CRC 校验
→ 若 compressed 则解压
→ found_cb(meta_blk*, payload, logical_size)
```

实现见 `meta_blk_service.cpp:1049-1112`、`1153-1203`。

压缩由 `write_meta_blk_internal` 控制：达到最小尺寸、压缩内存预算足够且压缩率满足阈值才启用。header 保存对齐后的存储尺寸、真实压缩长度和原始长度；恢复时先校验压缩字节，再解压。压缩指标在 `MetablkMetrics` 中。

限制：运行期 `read_sub_sb(type)` 当前不会透明解压，源码明确注释说明没有压缩客户端依赖此 API；压缩 metadata 应通过正常恢复 callback 路径读取。

## 依赖有序恢复

注册时的 `deps` 表示“当前 type 依赖这些 type”。内部建边：

```text
Dependency → CurrentType
```

恢复时先拓扑排序，再回放有依赖类型，最后回放独立类型；若依赖图有环，启动失败。实现见 `meta_blk_service.cpp:372-398`、`1118-1151`。

例如 A 依赖 B/C，B 依赖 D/E，C 依赖 F：

```text
D、E → B
F    → C
B、C → A
```

测试 `random_dependency_test` 验证 callback 与 completion callback 都遵守此顺序，见 `test_meta_blk_mgr.cpp:621-819`。

依赖图只规定恢复顺序，不提供跨 type 原子事务。

## 并发与停机

`m_meta_mtx` 保护 metadata 链、cache、注册信息和 add/update/remove/read；因此不同客户端可并发调用 API，但实际 metadata 修改全局串行。该模块面向低频控制面状态，优先一致性而非高吞吐。

`m_shutdown_mtx` 协调 `recover()` 和 `stop()`；恢复 completion callback 可调用 metadata API，故 recover 不能以全程持有 `m_meta_mtx` 的方式执行。

停机时服务释放普通 metadata cache、overflow header cache、handler、SSB buffer、压缩 buffer 和 META_VDEV 引用，见 `meta_blk_service.cpp:107-134`。

## 诊断与测试

- `get_status(log_level)`：输出 SSB、type、CRC、回调、record 数、block ID 与压缩状态。
- `dump_disk_metablks(client)`：重新扫描磁盘，必要时将 payload 导出到 `/tmp/dump_meta`。
- `verify_metablk_store()`：校验 SSB、普通链、prev/next、overflow chain、cache 与磁盘计数。

测试 `test_meta_blk_mgr.cpp` 覆盖小/大 metadata、写满、随机 add/update/remove/restart、依赖恢复、压缩回退、错误 header 兼容恢复以及重启后的 allocator block 占用重建。

## 关键取舍和限制

1. Meta Service 是控制面 superblock 存储，不适合高频大数据记录。
2. 所有修改由全局 mutex 串行，优先 crash consistency。
3. 同步写失败大多触发 release assert；根 metadata 损坏被视为系统级故障。
4. 大 payload 需要主 block、overflow headers、data blocks，更新有额外写放大。
5. type 字符串和 payload 格式均是持久化兼容协议；应由 payload 自身维护 magic/version。
6. `read_sub_sb` 的压缩透明读取当前不完整，应避免依赖。

## 总结

MetaBlkService 是 HomeStore 的 metadata 根服务：以 META_VDEV 中的 SSB 为入口，用双向 meta block 链组织所有 subsystem superblock；小数据内联、大数据走 overflow chain；通过“先写新数据、后发布引用”的顺序保证 add/update 的崩溃安全；通过链表扫描修复 remove 中断；通过 CRC、压缩、handler callback 和拓扑依赖顺序将恢复后的 metadata 交还给各子系统。
