# HomeStore LogService 设计与实现详解

`logservice` 是 HomeStore 的 WAL（write-ahead log）子系统，而不是日志打印模块。它为复制/RAFT、数据日志等上层提供持久化追加、批量提交、随机 LSN 映射、恢复、回滚和截断能力。核心目标是：让多个逻辑日志流复用一个物理 append-only 日志设备，获得高吞吐，同时保持崩溃恢复正确性。

## 代码入口

| 职责 | 文件 |
|---|---|
| 服务 API | `src/include/homestore/logstore_service.hpp` |
| 逻辑 LogStore API | `src/include/homestore/logstore/log_store.hpp` |
| 内部结构 | `src/include/homestore/logstore/log_store_internal.hpp` |
| 服务实现 | `src/lib/logstore/log_store_service.cpp` |
| LogDev、元数据与恢复 | `src/lib/logstore/log_dev.hpp`、`src/lib/logstore/log_dev.cpp` |
| Group Commit 编码 | `src/lib/logstore/log_group.cpp` |
| 流式恢复扫描 | `src/lib/logstore/log_stream.cpp` |
| NuRaft 适配 | `src/lib/replication/log_store/home_raft_log_store.cpp` |

## 整体架构

```text
HomeStore
  └── LogStoreService                    全局服务
        └── JournalVirtualDev            可增长 journal 虚拟设备
              ├── LogDev #1              一个物理顺序日志域
              │     ├── HomeLogStore A   逻辑日志流
              │     ├── HomeLogStore B
              │     └── ...
              └── LogDev #2
```

| 层 | 类 | 职责 |
|---|---|---|
| 服务层 | `LogStoreService` | 生命周期、LogDev ID、创建/打开/删除、统一物理截断 |
| 设备层 | `LogDev` | 汇聚 Store 请求、Group Commit、物理 IO、恢复扫描、持久化元数据 |
| 逻辑层 | `HomeLogStore` | LSN 语义、读写、连续性、逻辑截断、回滚 |
| 空间层 | `JournalVirtualDev` | 动态 chunk、append、truncate、跨重启重建 chunk 链 |

必须区分两套编号：

```text
LogDev global logid / idx: 0, 1, 2, ...   # 跨 Store 的物理顺序
LogStore LSN:                0, 1, 2, ... # 单个逻辑流自己的顺序
```

它们由 `logdev_key` 联系：

```cpp
struct logdev_key {
    logid_t idx;
    off_t dev_offset;
};
```

定义见 `log_store_internal.hpp:60-97`。每个逻辑 LSN 在内存中映射为：

```text
LSN -> { m_dev_key: 实际物理定位, m_trunc_key: 安全物理截断边界 }
```

对应 `logstore_record`，见 `log_store_internal.hpp:113-120`。

## 多 Store 复用一个 LogDev

系统以 group commit 提升吞吐：

```text
多个 HomeLogStore
      ↓
LogDev 的内存 StreamTracker
      ↓
LogGroup（混合多个 Store 的记录）
      ↓
sync_pwritev 到 JournalVirtualDev
```

物理记录可以交错：

```text
idx 0: Store 7 / LSN 0
idx 1: Store 9 / LSN 0
idx 2: Store 7 / LSN 1
```

这减少小 IO、提高设备利用率，但也意味着物理截断必须同时尊重共享该 LogDev 的所有 Store。

## 对外 API 与模式

`LogStoreService` 的核心接口见 `logstore_service.hpp:77-178`：

```cpp
create_new_logdev(flush_mode, pid)
open_logdev(logdev_id, flush_mode, pid)
destroy_log_dev(logdev_id)
create_new_log_store(logdev_id, append_mode)
open_log_store(logdev_id, store_id, append_mode, found_cb, replay_done_cb)
remove_log_store(logdev_id, store_id)
device_truncate()
```

全局入口是：

```cpp
LogStoreService& logstore_service() { return hs()->logstore_service(); }
```

位于 `log_store_service.cpp:37`。

`HomeLogStore` 提供 `write_async`、`append_async`、`write_and_flush`、`read_sync`、`flush`、`truncate`、`rollback` 和 `fill_gap`。

### 非 append 模式

调用方自己指定 LSN：

```cpp
store->write_async(lsn, blob, cookie, callback);
```

它可支持乱序 LSN、hole、重放和指定 index 的写入。

### append 模式

由 Store 自动分配 LSN：

```cpp
const auto lsn = m_next_lsn.fetch_add(1, std::memory_order_acq_rel);
store->append_async(blob, cookie, callback);
```

实现见 `log_store.cpp:85-93`。Raft Store 使用此模式，见 `home_raft_log_store.cpp:94-99`。

## 内存状态与连续性

`HomeLogStore` 的核心字段：

```cpp
m_start_lsn  // 当前可读起点
m_next_lsn   // append 模式下一个 LSN
m_tail_lsn   // 最大有效 LSN
m_records    // sisl::StreamTracker<logstore_record>
```

见 `log_store.hpp:291-301`。

`StreamTracker` 维护离散 LSN 的状态：hole、active（已发出未落盘）、completed、truncated、rollback。关键使用方式：

```cpp
m_records.create(lsn);
m_records.update(lsn, ...);
m_records.create_and_complete(lsn, record);
m_records.complete(from, upto);
m_records.truncate(upto);
m_records.rollback(to);
m_records.status(lsn);
```

`get_contiguous_issued_seq_num` 与 `get_contiguous_completed_seq_num` 返回连续前缀，而非最大 LSN：

```cpp
m_records.active_upto(from + 1)
m_records.completed_upto(from + 1)
```

见 `log_store.cpp:342-348`。例如已出现 `0,1,3,4` 时，最大编号为 4，但连续完成前缀只能到 1；复制协议依赖的是后者。

## 异步写路径

```text
HomeLogStore::write_async
  → m_records.create(LSN)
  → LogDev::append_async
  → m_log_records.create(global_idx, store_id, LSN, data, req)
  → flush_if_necessary / timer / 显式 flush
  → prepare_flush
  → LogGroup::add_record + finish
  → JournalVirtualDev::sync_pwritev
  → on_flush_completion
  → HomeLogStore::on_write_completion
  → completion callback
```

`HomeLogStore::write_async` 位于 `log_store.cpp:48-67`。它在 Store 的 StreamTracker 中创建 LSN 后，将请求转入 `LogDev`。

`LogDev::append_async` 位于 `log_dev.cpp:292-304`：

```cpp
const auto idx = m_log_idx.fetch_add(1, std::memory_order_acq_rel);
m_pending_flush_size.fetch_add(data.size(), std::memory_order_relaxed);
m_log_records->create(idx, store_id, seq_num, data, cb_context);
```

`idx` 是整个 LogDev 的顺序编号。请求尚未持久化，只存在于内存队列。

`logstore_req` 保存 `HomeLogStore*` 原始指针；调用方必须保证 Store 和数据 buffer 至少存活到完成回调。相关定义位于 `log_store_internal.hpp:123-158`。

## Flush 设计

`flush_mode_t` 是位图：

```cpp
INLINE   = 1 << 0
TIMER    = 1 << 1
EXPLICIT = 1 << 2
```

定义见 `log_store_internal.hpp:54-58`。

- `INLINE`：写入后按条件尝试提交。
- `TIMER`：定时检查提交条件。
- `EXPLICIT`：由上层 `flush` 或 `write_and_flush` 驱动。

`LogDev::flush_if_necessary`（`log_dev.cpp:440-493`）触发条件：

```cpp
flush_by_size = pending_sz >= threshold_size;
flush_by_time = !flush_by_size && pending_sz &&
                elapsed_time > max_time_between_flush_us;
```

`read_sync` 如果发现记录已经发出但未完成，会先执行 `flush_under_guard()`，见 `log_store.cpp:122-130`。因此 `read_sync` 可能执行同步写，要求当前线程支持同步 IO。

服务启动时创建专用 `log_flush_thread` reactor，见 `log_store_service.cpp:343-367`。不适合执行同步 IO 的线程会将 flush 转发给该 reactor。

### Flush 并发控制

真正 flush 使用 `m_flush_mtx` 串行化。`flush_if_necessary` 用 `try_lock`；失去锁竞争后会通过随机 worker 重试并携带 `force=true`：

```cpp
iomanager.run_on_forget(iomgr::reactor_regex::random_worker,
    [this, threshold_size] { flush_if_necessary(threshold_size, true); });
```

见 `log_dev.cpp:465-489`。这避免竞争者重置 `m_last_flush_time` 后，最后一个小写入被重新判断为“不需要 flush”而永久滞留。经由 worker 间接重投递还避免了向当前 flush reactor 直接投递时可能出现的同步递归和栈溢出。对应回归测试在 `test_log_store.cpp:1193-1279`。

## LogGroup 磁盘格式

物理落盘单位是 `LogGroup`：

```text
+--------------------+
| log_group_header   |
+--------------------+
| record descriptors |
+--------------------+
| inline data        |
+--------------------+
| OOB data buffers   |
+--------------------+
| log_group_footer   |
+--------------------+
```

记录描述符：

```cpp
struct serialized_log_record {
    uint32_t size;
    uint32_t offset : 31;
    uint32_t is_inlined : 1;
    logstore_seq_num_t store_seq_num;
    logstore_id_t store_id;
};
```

见 `log_dev.hpp:70-92`。它使恢复过程可以从混合 Group 中找回每条数据的 Store 与 LSN。

Group Header（`log_dev.hpp:125-177`）保存 magic、版本、记录数、起始 global idx、总尺寸、inline/OOB/footer 偏移、前一组 CRC、当前组 CRC 和 LogDev ID。

Footer（`log_dev.hpp:179-189`）保存 magic、版本和 `start_log_idx`。恢复必须同时验证 header、footer、CRC；单独存在 header 不代表本组完整持久化。

### Inline 与 OOB

`LogGroup::add_record` 位于 `log_group.cpp:75-114`。小记录、不对齐记录或地址不对齐记录会复制进对齐的 inline buffer；已对齐的大记录则直接作为 iovec 的 OOB 数据写出：

```cpp
m_iovecs.emplace_back(record.data.bytes(), record.data.size());
```

判断见 `log_dev.hpp:109-118`。这在减少复制和满足 DMA 对齐之间折中，但 OOB buffer 的生命周期必须覆盖 IO completion。

`LogGroup::finish` 添加 footer、对齐 iovec、填 header 并计算 CRC，见 `log_group.cpp:120-177`。CRC 覆盖 header 后的 inline 区、OOB 数据与 footer。

## 物理写入与完成

`LogDev::flush` 位于 `log_dev.cpp:515-576`：

```cpp
while (m_last_flush_idx < new_idx) {
    lg = prepare_flush(...);
    offset = m_vdev_jd->alloc_next_append_blk(lg->header()->total_size());
    m_vdev_jd->sync_pwritev(lg->iovecs().data(), ... , offset);
    on_flush_completion(lg);
}
```

单个 Group 的记录数有上限，故一次 flush 可以拆成多个 Group。`sync_pwritev` 成功之后，才开始更新完成状态并通知用户；这定义了写 callback 的 durability 语义。

`on_flush_completion`（`log_dev.cpp:578-648`）首先标记 LogDev 内存记录完成，再对每条记录调用：

```cpp
log_store->on_write_completion(req, {idx, dev_offset}, {group_start_idx, dev_offset});
```

前一个 key 用于读取定位，后一个 key 是 Group 起点，用于安全截断计算。

callback 可能并发且不保证按 append 顺序执行。无 replication service 时会投递至随机 worker；有 replication service 时直接同步执行。调用方不能依赖 callback 顺序，必须自行处理并发共享状态。

## 读取路径

`HomeLogStore::read_sync`（`log_store.cpp:116-146`）执行：

```text
检查 m_records.status(LSN)
  ├── hole / truncated → std::out_of_range
  ├── active           → 先 flush
  └── completed        → 用 m_dev_key 调 LogDev::read
```

`LogDev::read`（`log_dev.cpp:306-335`）持有 flush 锁，先读取 4 KiB 的 header/descriptor 区。如果记录不在首读 buffer 内，则再以设备对齐方式读取实际数据区。返回的 `sisl::byte_view` 持有底层缓冲区，不需要调用方释放。

读取会验证 magic、版本、idx 范围和尺寸关系；整个 Group 小于首读窗口时也校验 CRC。大型 Group 的完整 CRC 校验主要放在恢复路径，以避免单记录读的 IO 放大。

## 恢复设计

恢复依赖：持久化 metadata、Group header/footer、当前 CRC、相邻 Group CRC 链、chunk 链与 rollback metadata。

上层应先注册需要恢复的对象：

```text
open_logdev(...)
open_log_store(...)
LogStoreService::start(false)
```

`LogDev::start(false)`（`log_dev.cpp:53-111`）会：

```text
打开 Journal descriptor
→ 加载 LogDev metadata
→ 创建已由上层打开的 HomeLogStore
→ 从持久化起点 do_load
→ 重建 LSN 到物理位置的映射
→ 回调 log_found
→ 回调 replay_done
→ 对恢复出的截断边界做逻辑 truncate
```

### 元数据

`LogStoreService` 注册 `Logdev_sb`、`Logdev_rollback_sb`、`LogStoreServiceSB` 三类 meta block handler，见 `log_store_service.cpp:40-59`。

服务级 superblock 保存最新 LogDev ID。每个 LogDev superblock 保存 LogDev ID、flush mode、parent UUID、恢复物理起点、起始 global idx、已保留 Store 和每个 Store 的起始 LSN，结构见 `log_dev.hpp:396-429`。

### 流式扫描与完整性判断

`log_stream_reader` 位于 `log_stream.cpp`。它从 metadata 记录的 offset 依次验证：

```text
header magic
→ header LogDev ID
→ prev_grp_crc 与上一组 cur_grp_crc 的连续性
→ 数据是否完整读取
→ footer magic 与 start_log_idx
→ 当前 Group CRC
```

见 `log_stream.cpp:37-159`。

Journal chunk 可被复用但不会清零；因此 CRC 链可区分有效日志尾部与残留的旧数据。相关设计注释在 `journal_vdev.cpp:156-166`。

`LogDev::do_load`（`log_dev.cpp:213-274`）逐条解析记录；如果 global idx 不在 rollback 范围，就向对应 Store 调用 `on_log_found`，重建映射并执行上层重放 callback。最后更新 Journal descriptor tail，新的 append 从最终有效位置继续。

## 截断设计

截断分为逻辑截断和物理截断。

### 逻辑截断

```cpp
store->truncate(upto_lsn, true);
```

默认只更新 Store 的内存状态：

```cpp
m_records.truncate(upto_lsn);
m_start_lsn.store(upto_lsn + 1);
```

见 `log_store.cpp:208-259`。此后旧 LSN 不可读，但空间暂不回收。

### 安全截断 key

非 append 模式允许乱序 LSN。例如：

```text
Group 1: LSN 1, 4
Group 2: LSN 2, 3
```

截断到 LSN 3 时不能删除 Group 1，因为其中仍有有效的 LSN 4。因此 `m_trunc_key` 并不总是当前记录的实际位置：乱序旧 LSN 会继承 tail LSN 的安全 key。实现位于 `HomeLogStore::on_write_completion`，见 `log_store.cpp:148-186`。

### 物理截断

`LogStoreService::device_truncate` 遍历所有 LogDev。`LogDev::truncate`（`log_dev.cpp:650-725`）在持有 flush 锁、Store map 锁和 metadata 锁时：

```text
收集每个 Store 的 truncate_info
→ 取所有 Store 中最小的安全 logdev_key
→ JournalVirtualDev::truncate(offset)
→ 更新恢复起点 metadata
→ 清理已删除范围内 rollback record
→ 回收已删除 Store ID
→ 持久化 metadata
```

即：

```text
物理截断位置 = 所有非空 Store 安全截断位置的最小值
```

`TruncateAcrossMultipleStores` 测试（`test_log_dev.cpp:475-639`）覆盖了这一语义。资源管理器也会在周期审计或空间压力下调用 `device_truncate`，见 `resource_mgr.cpp:50-95`。

## 回滚

`HomeLogStore::rollback(to_lsn)`（`log_store.cpp:358-411`）用于复制日志分叉或覆盖写：

```text
校验范围
→ 如有在途写先 flush
→ 找出待撤销 LSN 对应的 global logid 范围
→ 回滚 Store 内存状态
→ 持久化 rollback range
```

rollback 不立即抹除物理数据，而在独立 metadata 中保存：

```cpp
struct rollback_record {
    logstore_id_t store_id;
    std::pair<logid_t, logid_t> idx_range;
};
```

见 `log_dev.hpp:431-472`。恢复扫描遇到该范围时不向相应 Store 重放。直到物理截断跨过这些日志，rollback metadata 才会被清除。

## 删除 Store 和延迟 ID 复用

`remove_log_store` 先将 Store 从内存 map 移除，再记录 `(当前 global logid - 1, store_id)` 到垃圾集合。只有物理截断越过该 global idx 时，才调用 `m_id_reserver->unreserve(store_id)`。

- 删除：`log_dev.cpp:797-813`
- 记录待回收 ID：`log_dev.cpp:381-388`
- 截断时实际回收：`log_dev.cpp:705-715`

这是必要的：若旧物理记录还存在就复用 ID，重启扫描可能把旧记录错误归属给新 Store。

## JournalVirtualDev

`LogStoreService::create_vdev` 创建动态大小、round-robin chunk、跨物理设备 striping 的 `LOGDEV_VDEV`，见 `log_store_service.cpp:67-98`。每个 LogDev 打开一个对应的 journal descriptor：

```cpp
m_vdev_jd = m_vdev->open(m_logdev_id);
```

重启时 `JournalVirtualDev` 根据 chunk 私有信息中的 `logdev_id`、`is_head`、`next_chunk`、`created_at` 重建每个 LogDev 的 chunk 链，并清理 orphan chunk，见 `journal_vdev.cpp:72-138`。

## NuRaft 适配

关系：

```text
nuraft::log_store
  └── HomeRaftLogStore
        └── ReplLogStore
```

Raft index 从 1 开始，LogStore LSN 从 0 开始：

```cpp
to_store_lsn(raft_lsn) = raft_lsn - 1
to_repl_lsn(store_lsn) = store_lsn + 1
```

见 `home_raft_log_store.cpp:39-45`。

`HomeRaftLogStore::append` 序列化 NuRaft entry 后调用 `append_async`，并维护一个 100 项 direct-mapped 读缓存，见 `home_raft_log_store.cpp:164-179`。

`end_of_append_batch` 会 flush 并推进 durable LSN，见 `home_raft_log_store.cpp:211-216`。

`write_at` 先 rollback 到前一条，再 append 替换记录并强制 flush，见 `home_raft_log_store.cpp:181-209`。

`compact` 调用 `truncate(..., false)`，要求直接进行物理截断；`ReplLogStore` 会将 Raft 的 compact 上限与数据面允许的截断上限取最小值，见 `repl_log_store.cpp:117-125`。

`ReplLogStore::end_of_append_batch` 的关键一致性规则是：对 follower 请求，先等待相关数据写入成功，后持久化 Raft 日志：

```text
data durable happens-before raft log durable
```

见 `repl_log_store.cpp:41-112`。否则进程重启后 replay 日志时可能无法确认关联数据是否真实落盘。

## 启动与停止顺序

Raft 服务中的启动顺序：

```text
恢复复制设备配置
→ open LogDev / LogStore
→ 启动 LogStoreService 并完成日志 replay
→ 启动 DataService
→ join Raft group
```

见 `raft_repl_service.cpp:162-210`。日志重放必须先于数据面启动，否则 allocator 可能重新分配尚未 replay 回来的已占用物理块。

停止时，服务层等待请求，LogDev 等待 callback，停止 Store，flush 剩余写入，再截断并停止 timer：

- `LogStoreService::stop`：`log_store_service.cpp:122-132`
- `LogDev::stop`：`log_dev.cpp:136-178`
- `HomeLogStore::stop`：`log_store.cpp:283-289`

## 指标与调试

`LogStoreServiceMetrics` 位于 `log_store_service.cpp:402-430`，包含 LogDev/Store 数量、读写次数、读写延迟、flush 尺寸、每组记录数、flush 后处理时延、flush 间隔和单条记录尺寸分布。

状态和诊断接口：

```cpp
LogStoreService::get_status()
LogDev::get_status()
HomeLogStore::get_status()
dump_log_store(log_dump_req)
```

`dump_log_store` 可导出 header 或完整数据，并重新从设备读取 completed record，见 `log_store.cpp:291-329`。

## 当前实现的注意项

1. `LogStoreService::close_log_store` 仍是 TODO（`logstore_service.hpp:145-148`）；当前没有真正“关闭但不删除”的 Store 生命周期操作。
2. `logstore_req::size()` 尚未实现，固定返回 0（`log_store_internal.hpp:140-144`）。
3. `HomeLogStore::flush(upto_lsn)` 当前忽略参数，实际 flush 整个共享 LogDev（`log_store.cpp:350-356`）。
4. append mode 的限制主要是 debug assert；release 构建中调用方不应依赖模式检查来阻止错误 API 使用。
5. callback 可能并发、无序，且 OOB buffer 需要调用方维持到 callback，使用方必须严格遵守这些异步契约。
6. 同一 LogDev 内最慢的 Store 会阻塞其他 Store 的物理空间回收；保留策略差异显著的业务应考虑分配不同 LogDev。
7. 部分错误早返回路径在增加 `pending_request_num` 后未显式递减，例如 `log_store_service.cpp:158-182`、`307-321` 和 `log_dev.cpp:306-335`；若错误路径可达，停机可能等待计数清零。适合改用 RAII guard。
8. `log_group_header::data()` 在 `log_dev.hpp:160-166` 使用 `start_log_idx - idx` 计算 descriptor 下标，但前置条件是 `idx >= start_log_idx`。该辅助函数当前未见调用，未来启用前应复查是否应为 `idx - start_log_idx`。

## 总结

HomeStore LogService 是一个以 `JournalVirtualDev` 为底座、以 `LogDev` 做批量物理提交、以 `HomeLogStore` 提供逻辑 LSN 语义的高吞吐 WAL 系统。它最重要的设计是：

1. **逻辑 Store 与物理 LogDev 分离**：LSN 语义和物理 append 顺序解耦。
2. **Group Commit**：跨 Store 聚合并同步写，降低小 IO 成本。
3. **两阶段截断**：先逻辑不可见，再按全体 Store 的最小安全边界回收物理空间。
4. **恢复优先正确性**：metadata 定位、footer 检测写完整性、CRC 与 CRC 链检测损坏和残留、rollback metadata 排除失效日志、回调驱动上层 replay。
