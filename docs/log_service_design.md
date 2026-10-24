# HomeStore Log Service 实现详解

这是一个 **group-commit 的 append-only WAL（Write-Ahead Log）系统**，专为 Raft 复制层等需要低延迟、高吞吐持久化日志的场景设计。下面按"从上到下、从写到读、从内存到磁盘"的顺序把整个实现串一遍。

---

## 一、整体分层架构

```
LogStoreService                     ← 全局单例，管理所有 LogDev，持有唯一的 JournalVirtualDev
      │
      ├── LogDev #1                 ← 一段独立的物理日志空间（一个 JournalVirtualDev::Descriptor）
      │      ├── HomeLogStore A      ← 逻辑日志流，自己的 lsn 空间
      │      ├── HomeLogStore B      ← 可以和 A 共享同一个 LogDev
      │      └── LogGroup pool[2]    ← group commit 用的双缓冲
      │
      └── LogDev #2 ...
            │
      JournalVirtualDev::Descriptor  ← 物理层：逻辑地址空间 ←→ chunk 链表
            │
      Chunk（物理设备上的一段连续空间，来自 ChunkPool 复用）
```

对应代码文件：
- `include/homestore/logstore_service.hpp` + `lib/logstore/log_store_service.cpp`
- `include/homestore/logstore/log_store.hpp` + `lib/logstore/log_store.cpp`
- `include/homestore/logstore/log_store_internal.hpp`
- `lib/logstore/log_dev.hpp/cpp`（核心）
- `lib/logstore/log_group.cpp`
- `lib/logstore/log_stream.cpp`
- `lib/device/journal_vdev.hpp/cpp`

---

## 二、关键标识符：两套独立的编号空间

| | `logstore_seq_num_t`（lsn） | `logid_t`（log_idx） |
|---|---|---|
| 归属 | 每个 `HomeLogStore` 各自独立 | 整个 `LogDev` 全局唯一 |
| 含义 | 业务层日志序号（如 Raft log index） | 物理 journal 中第几条写入 |
| 分配 | 调用方指定，或 append_mode 下 `m_next_lsn` 自动生成 | `LogDev::m_log_idx.fetch_add(1)` 原子生成 |
| 跨 store 关系 | 互不干扰 | 共享同一 LogDev 的多个 logstore **共用一个计数器**，物理上交叉编号 |

两者靠 `logdev_key{idx, dev_offset}` 绑定，而这张"lsn → logdev_key"的映射表存在 **每个 `HomeLogStore` 自己的 `m_records`** 里（后面详述）。

---

## 三、写路径（Write Path）全流程

### 3.1 从业务调用到全局 log_idx 分配

```cpp
// HomeLogStore::write_async (log_store.cpp)
m_records.create(req->seq_num);                                   // ① 在本 store 的 lsn 索引表里开槽
auto ret = m_logdev->append_async(m_store_id, req->seq_num, req->data, (void*)req);
```

```cpp
// LogDev::append_async (log_dev.cpp)
const auto idx = m_log_idx.fetch_add(1, std::memory_order_acq_rel);  // ② 全局原子分配 log_idx
m_pending_flush_size.fetch_add(data.size(), ...);
m_log_records->create(idx, store_id, seq_num, data, cb_context);     // ③ 按 idx 存：store_id+lsn+数据指针+req*
if (allow_inline_flush()) flush_if_necessary();
```

`LogDev::m_log_records`（按全局 idx 索引的 `StreamTracker<log_record>`）是个**临时中转站**：每个 slot 把 `(store_id, lsn, data, req指针)` 捆在一起，flush 完成后立刻被截断丢弃，不长期保存。

### 3.2 Flush 触发机制

`flush_mode_t` 三种模式可叠加：`INLINE`（写完立即触发）、`TIMER`（定时器周期触发）、`EXPLICIT`（显式调用）。

```cpp
bool LogDev::flush_if_necessary(int64_t threshold_size, bool force) {
    if (!can_flush_in_this_thread()) {
        iomanager.run_on_forget(logstore_service().flush_thread(), [...]{ flush_if_necessary(...); });
        return false;
    }
    bool flush_by_size = (pending_sz >= threshold_size);
    bool flush_by_time = !flush_by_size && pending_sz && (elapsed > max_time_between_flush_us);
    if (force || flush_by_size || flush_by_time) {
        std::unique_lock lck(m_flush_mtx, std::try_to_lock);
        if (lck.owns_lock()) return flush();
        // 抢锁失败：不能静默放弃，必须 force=true 重新调度重试，
        // 否则最后一笔写入在 flush 定时器关闭的场景下可能永远得不到 flush（已修复的 hang bug）
        iomanager.run_on_forget(random_worker, [...]{ flush_if_necessary(threshold_size, true); });
    }
}
```

按**数据量阈值**或**距上次 flush 的时间阈值**决定是否 flush，`try_lock` 抢不到锁时必须强制重试，不能静默放弃（否则会在禁用定时器的测试场景下产生永久 hang）。

### 3.3 LogGroup 双缓冲池（Group Commit 的核心）

```cpp
static constexpr uint32_t max_log_group{2};
LogGroup m_log_group_pool[max_log_group];   // 启动时一次性预分配好 DMA 对齐内存，之后反复 reset() 复用
uint32_t m_log_group_idx{0};

LogGroup* make_log_group(uint32_t n) { m_log_group_pool[m_log_group_idx].reset(n); return &m_log_group_pool[m_log_group_idx]; }
void free_log_group(LogGroup* lg) { m_log_group_idx = !m_log_group_idx; }   // 纯粹的 0/1 翻转
```

为什么是 2 个：`m_flush_mtx` 是显式设计为**同线程可重入**的 fiber 锁（"if we support inline flush mode, we might schedule flush operation in the same thread...so we need the lock to be locked for multiple times in the same thread"）。原因是 `on_flush_completion` 的完成回调可能是**同步**执行的（有复制服务时 `callback_lambda()` 直接调用，而不是丢到 worker 异步执行），而回调里常常会立刻触发下一次写入 → 再次 inline flush → **在还没从外层 `flush()` 返回时递归进入 `flush()`**。`free_log_group` 在执行回调**之前**就把槽位翻转好，让这次递归重入能拿到另一块独立缓冲区，不与外层正在收尾的那块冲突。代码假设嵌套深度最多为 2 层，这是文档化的设计前提。

此外 `flush()` 自身的 for 循环如果一次要处理的记录数超过单个 LogGroup 的上限，也会在相邻两轮之间交替使用这两个槽位。

### 3.4 LogGroup 的组装（`prepare_flush` → `add_record` → `finish`）

```cpp
LogGroup* LogDev::prepare_flush(int32_t estimated_records) {     // estimated = 积压量 + 4 冗余
    auto* lg = make_log_group(estimated_records);
    m_log_records->foreach_contiguous_active(m_last_flush_idx + 1, [&](idx, _, record) {
        if (lg->add_record(record, idx)) { flushing_upto_idx = idx; return true; }
        else return false;   // 这个 group 满了，停止遍历
    });
    lg->finish(m_logdev_id, m_last_crc);
    ...
}
```

**一个 LogGroup 实际包含多少条记录是动态的**（取决于当次积压量），但有**硬上限**：

```cpp
static constexpr uint32_t max_records_in_a_batch{
    (initial_read_size /*4096*/ - sizeof(log_group_header)) / sizeof(serialized_log_record)
};   // 约 200 条，编译期常量
m_max_records = std::min(estimated_records, max_records_in_a_batch);
```

这个上限纯粹是为了配合读路径优化：保证 `LogDev::read()` 第一次固定读 4KB 时，一定能把整个"header + 记录元数据数组"读全，不需要先猜大小再读一次。超过上限时，`add_record` 返回 `false`，`flush()` 的外层 for 循环继续用另一个槽位组装下一个 LogGroup——一次 `flush()` 调用可能产生多个 LogGroup。

**单条记录的数据大小完全不固定**，由调用方的 `sisl::io_blob` 决定：

```cpp
bool log_record::is_inlineable(flush_size_multiple) const {
    return is_size_inlineable(data.size(), flush_size_multiple)       // 小于配置阈值，或
        || (uintptr_t(data.cbytes()) % flush_size_multiple != 0)      // buffer地址没对齐，或
        || !data.is_aligned();                                        // buffer本身没对齐
}
```

- **inline（内联）**：小数据/未对齐数据，`memcpy` 拷进 LogGroup 缓冲区，紧密排列，无 padding；
- **oob（out-of-band）**：大且 DMA 对齐的数据，直接作为独立 iovec 零拷贝引用用户 buffer，省一次内存拷贝。

每条记录还对应一个**固定大小**的元数据槽位 `serialized_log_record`（~20 字节：size、offset+inlined位、store_seq_num、store_id），写入时通过 `m_record_slots[m_nrecords]` 顺序填充——这个指针就是指向 LogGroup 缓冲区里紧跟在 `log_group_header` 后面的那段元数据数组：

```cpp
m_record_slots = reinterpret_cast<serialized_log_record*>(m_cur_log_buf + sizeof(log_group_header));
```

### 3.5 LogGroup 物理布局

```
| Log Group Header | Record_slot[0] | Record_slot[1] | ... | inline data area | oob data area | Footer |
```

- `log_group_header`：magic、version、n_log_records、start_log_idx（组内第 0 号 slot 对应的全局 idx）、各区域偏移、**`prev_grp_crc` + `cur_grp_crc`（链式 CRC）**、logdev_id；
- 每个 `serialized_log_record` 槽位：记录大小、offset（inline 则是组内绝对偏移，oob 则要再加上 `oob_data_offset`）、inline 标记、`store_id`+`store_seq_num`（这是"物理 idx ←→ 逻辑 lsn"映射能够持久化、能够在 recovery 时重建的根本原因）；
- `log_group_footer`：记录 `start_log_idx`，用来判断这个 group 是否被**完整写完**（如果 crash 在写一半，footer 和 header 的 `start_log_idx` 对不上）。

落盘用一次 `sync_pwritev`（iovec 聚合写，减少系统调用/寻址开销），这也是为什么大块 oob 数据能做到零拷贝——它本身就是一个单独的 iovec 元素，直接指向用户 buffer。

### 3.6 Flush 完成回调

```cpp
void LogDev::on_flush_completion(LogGroup* lg) {
    m_log_records->complete(lg->m_flush_log_idx_from, lg->m_flush_log_idx_upto);
    for (idx in [from, upto]) {
        auto& record = m_log_records->at(idx);
        req = (logstore_req*)record.context;          // 取回 append_async 时存的 context
        log_store->on_write_completion(req, logdev_key{idx, dev_offset}, logdev_key{from_indx, dev_offset});
        req_map[idx] = req;
    }
    free_log_group(lg);                 // 槽位翻转（在回调执行前！）
    m_log_records->truncate(upto_indx); // 已 flush 的临时索引立刻丢弃
    for (auto [idx, req] : req_map) {
        if (hs()->has_repl_data_service()) callback_lambda();           // 同步执行（可能触发递归 flush）
        else iomanager.run_on_forget(random_worker, callback_lambda);   // 异步执行
    }
}
```

`HomeLogStore::on_write_completion`（真正把 lsn→logdev_key 固化下来）：

```cpp
void HomeLogStore::on_write_completion(logstore_req* req, const logdev_key& ld_key, const logdev_key& flush_ld_key) {
    logdev_key trunc_key;
    if (m_tail_lsn < req->seq_num) { m_tail_lsn = req->seq_num; trunc_key = flush_ld_key; }
    else { trunc_key = m_records.at(m_tail_lsn).m_trunc_key; }   // 乱序完成场景，见下文
    atomic_update_max(m_next_lsn, req->seq_num + 1, ...);
    m_records.update(req->seq_num, [&](logstore_record& rec){ rec.m_dev_key = ld_key; rec.m_trunc_key = trunc_key; return true; });
}
```

**支持乱序 lsn 完成**：允许先写 lsn=1，再写 lsn=4，再写 lsn=2 并全部 flush——这是专门给外部复制引擎（非 Raft 默认路径）用的扩展能力，`m_trunc_key` 的设计保证了"即使乱序完成，truncate 时也不会把还包含高 lsn 的 LogGroup 误删"。

---

## 四、读路径：从 (logstore, lsn) 到物理字节

### 4.1 第一步：内存查表

```cpp
// HomeLogStore::read_sync
auto s = m_records.status(seq_num);
if (s.is_out_of_range || s.is_hole) throw std::out_of_range(...);   // 已截断或从未写过
else if (!s.is_completed) m_logdev->flush_under_guard();             // 还没flush，先强制flush
const logdev_key ld_key = m_records.at(seq_num).m_dev_key;           // O(1) 查表
```

### 4.2 第二步：读取整个 LogGroup 的 header（第一次物理 IO）

```cpp
log_buffer LogDev::read(const logdev_key& key) {
    auto buf = make_byte_array(4096, ...);
    m_vdev_jd->sync_pread(buf->bytes(), 4096, key.dev_offset);    // ★ Descriptor::offset_to_chunk 定位物理chunk
    auto* header = (log_group_header*)buf->cbytes();
    verify_log_group_header(key.idx, header);                    // 校验 magic/version/idx范围/(CRC若整组≤4K)
}
```

`offset_to_chunk` 把"逻辑 offset"转成"落在哪个 chunk、chunk 内偏移是多少"——这是全链路中唯一一处"查 chunk"的地方。

### 4.3 第三步：组内定位（纯指针运算，零查找）

```cpp
auto record_header = header->nth_record(key.idx - header->start_log_idx);  // 按下标直接定位
uint32_t data_offset = record_header->offset + (record_header->get_inlined() ? 0 : header->oob_data_offset);
```

因为组内记录严格按 log_idx 连续排列，`idx - start_log_idx` 直接就是数组下标。

### 4.4 第四步：按需二次读取

```cpp
if ((data_offset + record_header->size) < initial_read_size) {
    ret_view = byte_view{buf, data_offset, record_header->size};    // 数据已在首次4K内，直接切片，零额外IO
} else {
    // 数据超出首次读范围，按对齐边界精确计算，发起第2次 sync_pread
    ...
}
```

---

## 五、Recovery（崩溃恢复）

`LogDev::start(format=false)` → `do_load(offset)`：

1. 从 `LogDevMetadata` 加载 logdev 级 superblk（已注册的 store id 集合、起始 offset/log_idx、rollback 记录）；
2. 用 `log_stream_reader` 从上次已知的起点顺序扫描整条物理日志；
3. 每解析出一个合法 LogGroup，遍历其内所有记录，逐条调 `on_logfound(store_id, lsn, {idx, offset}, flush_ld_key, data, ...)`；
4. 跳过已被 `rollback` 标记的 log_idx；
5. 最终调用 `HomeLogStore::on_log_found`，**把 `m_records` 从零重建起来**：

```cpp
void HomeLogStore::on_log_found(seq_num, ld_key, flush_ld_key, buf) {
    m_records.create_and_complete(seq_num, logstore_record(ld_key, trunc_key));
    atomic_update_max(m_next_lsn, seq_num + 1, ...);
    if (m_found_cb) m_found_cb(seq_num, buf, nullptr);
}
```

**这证明了 `HomeLogStore::m_records` 本身从不持久化**——它是一个可以从 journal 物理数据完全重放重建的内存索引，真相只存在于磁盘上每条记录自带的 `(store_id, store_seq_num)` 标签里。

`log_stream_reader::next_group`（`log_stream.cpp`）靠**三重校验**判断数据有效性：magic word、CRC 链（`prev_grp_crc` 必须等于上一个读到的 group 的 `cur_grp_crc`）、footer 完整性（`footer->start_log_idx == header->start_log_idx`），任一失配即认为到达日志尾部（除非明确不在最后一个 chunk 上才判定为真正的数据损坏并 assert）。

---

## 六、两级截断 + chunk 回收

### 6.1 逻辑截断（`HomeLogStore::truncate`）

只更新内存：

```cpp
m_trunc_ld_key = m_records.at(upto_lsn).m_trunc_key;
m_records.truncate(upto_lsn);        // StreamTracker 窗口左边界前移
m_start_lsn.store(upto_lsn + 1);
```

### 6.2 物理截断（`LogDev::truncate`）

遍历该 LogDev 下**所有** logstore 的 `truncate_info()`，取**最小**的安全 `logdev_key`（所有 store 里还没截断的最老数据），只有这个最小值对应的物理位置才能真正截断：

```cpp
for (auto& [store_id, store] : m_id_logstore_map) {
    auto [trunc_lsn, trunc_ld_key, tail_lsn] = lstore->truncate_info();
    if (trunc_ld_key.idx < min_safe_ld_key.idx) min_safe_ld_key = trunc_ld_key;
}
m_vdev_jd->truncate(min_safe_ld_key.dev_offset);   // 真正物理截断
```

这保证共享同一 LogDev 的多个 logstore 不会互相误删对方还需要的数据。删除的 logstore id 也不会立刻复用，而是放进 `m_garbage_store_ids`，等物理截断推进到对应位置后才真正 `unreserve`。

### 6.3 物理层：`JournalVirtualDev::Descriptor::truncate`

```cpp
auto [new_head_chunk, index, offset_in_chunk] = offset_to_chunk(truncate_offset);
new_head_chunk->is_head = true;             // ★ 先持久化新head（crash-safe关键一步）
// 遍历旧链表头部，完全落在 truncate_offset 之前的 chunk 摘除、归还 chunk 池
for (...) { if (fully_before(truncate_offset)) { erase; release_chunk_to_pool(chunk); } }
update_data_start_offset(truncate_offset);  // 窗口左边界前移
```

**先写新 head 再清理旧 chunk**的顺序是故意的：如果中途 crash，磁盘上会同时存在两个 `is_head=true` 的 chunk，`JournalVirtualDev::init()` 用 `created_at` 取最新的那个，天然容错，不需要额外的原子提交机制。

---

## 七、Rollback

```cpp
bool LogDev::rollback(logstore_id_t store_id, logid_range_t id_range) {
    m_logdev_meta.add_rollback_record(store_id, id_range, true /*立即持久化*/);
}
```

记录一段 `[from, to]` 的 log_idx 为"该 logstore 已失效"，立即持久化到 `rollback_superblk`；recovery 时 `is_rolled_back` 命中则直接跳过、不回调给 logstore——给 Raft 这类需要"截断未提交尾部日志"的场景用，比真的物理删除便宜得多。`HomeLogStore::rollback` 在逻辑层会先强制 flush 掉回滚点之后的未决写入，再回退 `m_tail_lsn`/`m_next_lsn` 并清理 `m_records`。

---

## 八、内存占用：`m_records` 是滑动窗口，不是全历史表

`HomeLogStore::m_records` 底层是 `sisl::StreamTracker<logstore_record>`：

- 本质是一个**连续内存数组**（`calloc` 分配）+ 两个 bitset（是否创建/是否完成），通过 `idx - m_slot_ref_idx` 做 O(1) 下标访问，不是 hashmap；
- 写入新 idx 超出当前容量时 `do_resize` 翻倍扩容（类似 `std::vector`）；
- `truncate()` 会**立刻**前移 `m_slot_ref_idx`（逻辑窗口左边界），之前的 lsn 立刻变得"out_of_range"，但底层 buffer 的物理 `memmove` 压缩是懒惰的（攒到 `m_data_skip_count > 5000` 才搬一次，摊销开销）；
- **内存占用 ≈ 32字节（`sizeof(logstore_record)`）× 当前未截断窗口宽度**，而不是 × 历史总日志条数；
- 窗口宽度完全取决于"业务层截断跟不跟得上写入节奏"——Raft 正常提交+compaction 场景下窗口很小（几百到几万条），但如果截断长期滞后，窗口会无限增长；
- 有个"高水位不回落"的细节：一旦因为积压把 buffer 撑大过，truncate 之后逻辑窗口变小了，但底层物理 buffer 容量不会自动收缩，会一直占用到对象销毁或下次扩容。

对比 `LogDev::m_log_records`（全局 idx 索引），它的窗口 = "已追加但还没 flush 的记录数"，每次 flush 完成立刻截断，天然很小，和日志历史总量无关。

---

## 九、JournalVirtualDev：物理存储层

### 9.1 Descriptor = 一个 logdev 的"文件描述符"

滑动窗口式的 chunk 链表：`m_data_start_offset`（左边界，truncate 推进）、`m_end_offset`（右边界，只增不减，按 chunk 整块扩展）。逻辑地址空间**永不回绕**。

### 9.2 chunk 自带元数据，天然持久化链表

```cpp
struct JournalChunkPrivate {
    logdev_id_t logdev_id; bool is_head; uint64_t created_at;
    uint64_t end_of_chunk;   // 该chunk实际写到哪就不再写（换新chunk后留下的"洞"）
    chunk_num_t next_chunk;  // 链表指针，存在chunk自己的私有元数据区
};
```

`JournalVirtualDev::init()` 启动时扫描所有 chunk，找 `is_head` 最新的作为链表起点，顺着 `next_chunk` 重建整条链；没被任何链表访问到的是孤儿 chunk，直接清理。

### 9.3 分配与写入

```cpp
off_t alloc_next_append_blk(size_t sz) {
    RELEASE_ASSERT_LT(sz, chunk_size, ...);     // 这是单条记录实际大小的硬上限来源
    if (tail_offset() + sz >= m_end_offset) append_chunk();   // 空间不够，接新chunk
    m_reserved_sz += sz;
    return tail_offset();
}
```

两阶段写入：先 `alloc_next_append_blk` 预留逻辑 offset，再 `sync_pwritev` 真正落盘。写入不能跨 chunk 边界（由 `alloc_next_append_blk` 保证）。

### 9.4 chunk 池复用

释放的 chunk **不清零**（代价太高），只清掉私有元数据丢进池子复用，靠 recovery 时的 magic/CRC 校验识别"这是旧数据还是有效数据"。

---

## 十、并发控制与优雅关闭

三层（`LogStoreService`/`LogDev`/`HomeLogStore`）都有同一套模式：

```cpp
std::atomic_bool m_stopping{false};
mutable std::atomic_uint64_t pending_request_num{0};
bool is_stopping() const { return m_stopping.load(); }
void start_stopping() { m_stopping = true; }
```

`stop()` 先置位拒绝新请求，再自旋等待 in-flight 请求计数清零，再等 flush 相关的 pending callback 清零，最后做一次 truncate 收尾。关键锁：`m_store_map_mtx`（保护 logstore 注册表）、`m_flush_mtx`（保护 flush 临界区，可重入）、`m_stream_tracker_mtx`（保护 flush 完成回调和并发 append 之间，`StreamTracker` 扩容搬迁时的内存安全）。

多个 logstore 共享一个 LogDev 时**写入不能并发**（"logdev doesn't support concurrent writes"），靠 group commit + 双缓冲提升吞吐，而不是靠并行写。

---

## 十一、整体数据流总图

```
写入:
  HomeLogStore.write_async(lsn, data)
    → m_records.create(lsn)                              [本store的lsn索引表开槽]
    → LogDev.append_async(store_id, lsn, data)
        → m_log_idx++ 分配全局idx
        → m_log_records[idx] = {store_id, lsn, data, req*}  [临时中转]
        → flush_if_necessary()
            → LogDev.flush()
                → prepare_flush: 从 m_log_records 里取出待刷新记录，逐条 LogGroup.add_record
                    → 小/未对齐数据inline拷贝；大/对齐数据oob零拷贝引用
                → LogGroup.finish(): 算CRC、填header/footer
                → JournalVirtualDev.alloc_next_append_blk(group总大小) → 逻辑offset
                → sync_pwritev(iovecs, offset)
                    → offset_to_chunk(offset) → 物理chunk+偏移 → 物理IO
                → on_flush_completion:
                    → HomeLogStore.on_write_completion(req, logdev_key{idx,offset})
                        → m_records.update(lsn): 写入 {m_dev_key, m_trunc_key}  [持久映射，仅内存]
                    → m_log_records.truncate(已flush部分)  [临时中转站清空]
                    → 执行用户回调

读取:
  HomeLogStore.read_sync(lsn)
    → m_records.at(lsn) → logdev_key{idx, dev_offset}
    → LogDev.read(logdev_key)
        → sync_pread(4K @ dev_offset) → offset_to_chunk → 物理IO 拿到 LogGroup header
        → verify header（magic/version/idx范围/[CRC]）
        → header.nth_record(idx - start_log_idx) → 定位到具体记录的元数据
        → 算data_offset（inline直接用，oob加oob_data_offset）
        → 数据在4K内？直接切片 : 二次精确对齐读取
    → 返回 log_buffer

恢复:
  LogDev.start(format=false)
    → 加载 LogDevMetadata（store列表、起始offset/idx、rollback记录）
    → do_load: log_stream_reader 顺序扫描每个LogGroup
        → 校验 magic/CRC链/footer
        → 跳过rollback范围内的记录
        → on_logfound(store_id, lsn, {idx,offset}) → HomeLogStore.on_log_found
            → m_records.create_and_complete(lsn, {ld_key, trunc_key})  [从零重建索引]

截断:
  HomeLogStore.truncate(upto_lsn): 逻辑截断，m_records窗口左移
  LogDev.truncate(): 取所有store里最小安全logdev_key → JournalVirtualDev.truncate(offset)
    → 定位新head chunk并持久化is_head=true（crash-safe） → 回收旧chunk到池子 → 左边界前移
```

---

## 十二、设计哲学小结

1. **WAL 是唯一真相来源，内存索引全部可重建**：`HomeLogStore::m_records`、`LogDev::m_log_records` 都是纯内存、可从物理日志重放重建的缓存索引，不需要额外保证"索引和数据"的一致性问题。
2. **Group commit 换吞吐**：多条记录攒批、一次 iovec 聚合写落盘，大数据走零拷贝 oob 路径，小数据走内联拷贝，兼顾吞吐和小写延迟。
3. **双缓冲池支撑 inline-flush 的同步递归场景**，用最小的代价（2个预分配buffer+可重入锁）解决了一个具体的、有限深度的重入问题，而不是上通用对象池。
4. **逻辑地址空间与物理 chunk 管理完全解耦**：`LogDev` 只认识线性递增的逻辑 offset，`JournalVirtualDev` 负责把它映射到可动态伸缩、可复用的 chunk 链表，chunk 元数据自带链表指针实现免维护的持久化。
5. **内存占用和截断节奏强绑定**：整个系统没有对"未截断窗口"做硬性大小限制，完全依赖上层（Raft/ResourceMgr）及时截断来约束内存和磁盘占用，这是一种"信任上层、不做过度防御"的设计取舍。
6. **crash-safe 靠幂等重建而非原子事务**：无论是 chunk 的 head 切换、还是 journal 内容的完整性校验（magic/CRC链/footer 三重验证），全都是"允许中间态存在，靠重启时的确定性规则消歧义"，而不是依赖复杂的两阶段提交协议。
