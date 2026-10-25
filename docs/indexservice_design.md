# HomeStore IndexService 设计与实现详解

`IndexService` 是 HomeStore 的持久化 B+Tree 索引服务，不是具体业务索引。它通过 `IndexTable<K,V>` 为上层提供 BTree 节点分配、缓存、写回、checkpoint、元数据和崩溃恢复能力。

其设计目标是：正常读写优先走内存和异步 checkpoint；节点页依赖原子写；split、merge 和 root 切换通过依赖 DAG 和 checkpoint journal 保证崩溃后结构可恢复。

> **持久化边界**：IndexService 保证此前已经持久化的数据不丢、BTree 结构能够恢复为可继续读写的状态；它不保证当前尚未完成 checkpoint 的最近业务更新全部保留。业务层如需强 durability，须维护自己的 journal 并在崩溃后重放。设计原则见 `src/lib/index/README.md:3-7`。

## 代码位置

| 模块 | 文件 |
|---|---|
| 服务 API | `src/include/homestore/index_service.hpp` |
| 内部结构 | `src/include/homestore/index/index_internal.hpp` |
| 通用索引表 | `src/include/homestore/index/index_table.hpp` |
| 服务实现 | `src/lib/index/index_service.cpp` |
| 写回缓存 | `src/lib/index/wb_cache.hpp`、`src/lib/index/wb_cache.cpp` |
| CP 上下文和 journal | `src/lib/index/index_cp.hpp`、`src/lib/index/index_cp.cpp` |
| 恢复原理说明 | `src/lib/index/README.md` |
| 常规测试 | `src/tests/test_index_btree.cpp` |
| 崩溃恢复测试 | `src/tests/test_index_crash_recovery.cpp` |
| GC 测试 | `src/tests/test_index_gc.cpp` |

## 整体架构

```text
业务层
  │ IndexTable<K,V>::put/get/remove/query
  ▼
IndexTable<K,V>
  │ 继承 Btree<K,V>；实现节点存储接口
  ▼
IndexWBCache
  ├─ SimpleCache<BlkId, BtreeNodePtr>
  ├─ dirty node 列表
  ├─ checkpoint transaction journal
  ├─ 节点持久化依赖 DAG
  └─ 并行 checkpoint flush fibers
  ▼
VirtualDev（INDEX_VDEV，固定块分配）
  ▼
物理设备 / chunk
```

表的 superblock 和 WBCache 的事务 journal 由 `MetaService` 管理：

```text
IndexTable superblock: UUID、ordinal、root、depth、节点统计、用户 SB、chunk 信息
WBCache journal:     当前/上次未完成 checkpoint 的结构变更事务
```

## 三个核心对象

### `IndexService`

`IndexService` 是全局管理层，入口为：

```cpp
IndexService& index_service() { return hs()->index_service(); }
```

实现见 `src/lib/index/index_service.cpp:33`。

它管理 VDev、全局 WBCache、table ordinal、UUID/ordinal 到表对象的映射和恢复转发：

```cpp
std::unique_ptr<IndexServiceCallbacks> m_svc_cbs;
std::unique_ptr<IndexWBCacheBase> m_wb_cache;
std::shared_ptr<VirtualDev> m_vdev;
std::unique_ptr<sisl::IDReserver> m_ordinal_reserver;
std::map<uuid_t, std::shared_ptr<IndexTableBase>> m_index_map;
std::unordered_map<uint32_t, std::shared_ptr<IndexTableBase>> m_ordinal_index_map;
```

见 `index_service.hpp:44-58`。

### `IndexTable<K,V>`

```cpp
class IndexTable : public IndexTableBase, public Btree<K,V>
```

定义于 `index_table.hpp`。它将通用 BTree 算法接到 IndexService 提供的物理存储：

| BTree 动作 | 适配实现 |
|---|---|
| 分配 node | `alloc_node` → `wb_cache().alloc_buf` |
| 读取 node | `read_node_impl` → `wb_cache().read_buf` |
| 标脏/写 node | `write_node_impl` → `wb_cache().write_buf` |
| 获取可写 buffer | `refresh_node` → `wb_cache().get_writable_buf` |
| split/merge 事务 | `transact_nodes` → `wb_cache().transact_bufs` |
| 释放 node | `free_node_impl` → `wb_cache().free_buf` |
| root 切换 | `on_root_changed` → metadata buffer transaction |

`IndexTableBase` 消除了模板类型，允许服务层按 ordinal 对任意表执行修复、更新 superblock、审计和停止。接口定义在 `index_internal.hpp:85-104`。

### `IndexWBCache`

`IndexWBCache` 同时是节点 cache、writeback cache、CP flush 调度器、依赖 DAG 管理器和崩溃恢复器。核心成员见 `wb_cache.hpp:35-45`：

```cpp
std::shared_ptr<VirtualDev> m_vdev;
sisl::SimpleCache<BlkId, BtreeNodePtr> m_cache;
std::vector<iomgr::io_fiber_t> m_cp_flush_fibers;
std::mutex m_flush_mtx;
void* m_meta_blk;
bool m_in_recovery;
```

## Index VDev

`IndexService::create_vdev` 创建 `INDEX_VDEV`，见 `index_service.cpp:51-67`。关键参数：

```cpp
.blk_size = atomic_page_size,
.alloc_type = blk_allocator_type_t::fixed,
.multi_pdev_opts = vdev_multi_pdev_opts_t::ALL_PDEV_STRIPED
```

因此：

- 一个 BTree node 占一个固定 block；
- node size 等于设备 atomic page size；
- `bnodeid_t` 可映射为 `BlkId`；
- 单 node 的原地写依赖原子页写，不产生 torn page；
- 多盘可 striping。

Index VDev 支持自定义 `ChunkSelector`。节点分配时将 table ordinal 写入 allocation hint：

```cpp
hints.application_hint = ordinal;
m_vdev->alloc_contiguous_blks(1, hints, blkid);
```

见 `wb_cache.cpp:95-124`。这使上层可按 table/tenant/分区决定 chunk 选取策略。

## 表 superblock

`index_table_sb` 定义于 `index_internal.hpp:43-78`，主要字段：

```text
magic / version
uuid / parent_uuid
root_node / root_link_version
index_size
total_leaf_nodes / total_interior_nodes / btree_depth
ordinal
user_sb_size + user_sb_bytes
pdev_id / index_num_chunks / max_size_bytes / chunk IDs
```

其中 root、link version 与 depth 是恢复的关键；`parent_uuid` 由业务层控制，可用于归属和故障隔离。

创建表时：

```text
分配或保留 ordinal
→ 创建、填写并写 table superblock
→ 创建 MetaIndexBuffer
→ 获取 CP guard
→ 创建第一个 root leaf
```

实现见 `index_table.hpp:63-93`。

若创建过程崩溃，重启时发现 superblock 的 `root_node == empty_bnodeid`，则 `recovery_completed` 会创建新 root，见 `index_table.hpp:105-115`。

## IndexTable 注册与恢复回调

新表创建后，上层需显式注册：

```cpp
hs()->index_service().add_index_table(table);
```

服务同时维护：

```text
UUID    → IndexTable
ordinal → IndexTable
```

见 `index_service.cpp:170-178`。

恢复时，IndexService 不知道具体 `K/V` 类型，因此必须使用 `IndexServiceCallbacks`：

```cpp
virtual std::shared_ptr<IndexTableBase>
on_index_table_found(superblk<index_table_sb>&&) = 0;
```

回调负责根据表 metadata 创建正确模板类型和 BTreeConfig。测试示例见 `test_index_btree.cpp:71-83`。

## 节点缓存与跨 checkpoint 内存版本

WBCache cache 的 key 是 `BlkId`，value 是 `BtreeNodePtr`。只有 clean node 可以被逐出：

```cpp
return node->m_idx_buf->is_clean();
```

见 `wb_cache.cpp:48-55`。

读取流程：先查 cache，miss 时从 VDev 同步读；并发 miss 的两个线程中，插入失败者重试读取 cache，见 `wb_cache.cpp:145-167`。

节点采用**物理原地更新**，但为支持并发 checkpoint，使用**内存 copy-on-write buffer**：

```text
CP N:   旧 buffer 正在 dirty/flush
CP N+1: 若要修改同一节点，深拷贝新 IndexBuffer
        但二者对应同一个 BlkId
```

`get_writable_buf` 在旧 buffer 未 clean 时复制其字节，并将 BTreeNode 的物理 buffer 指针切换到新 buffer，见 `wb_cache.cpp:169-198`。

若 BTree 操作发现 CP 不匹配，`IndexTable::put/remove` 重新获取 CP guard 并重试，见 `index_table.hpp:163-196`。

## IndexBuffer 与持久化依赖 DAG

`IndexBuffer` 定义在 `index_internal.hpp:114-163`。关键状态：

```cpp
CLEAN → DIRTY → FLUSHING → CLEAN
```

关键字段：

```cpp
m_blkid;
m_dirtied_cp_id;
m_created_cp_id;
m_up_buffer;
m_wait_for_down_buffers;
m_index_ordinal;
m_is_meta_buf;
m_node_freed;
```

- `m_up_buffer` 是必须在当前 buffer 之后持久化的上游节点，通常为父节点；
- `m_wait_for_down_buffers` 是当前节点需等待的下游依赖数；
- 因此持久化顺序是 **down/child/new node → up/parent → meta superblock**。

对 split：

```text
P10 依赖 C10
C10 依赖 C15(new)

flush: C15 → C10 → P10
```

这避免 P10 先指向未落盘的 C15，也避免 C10 已丢失半数条目、C15 却尚未可达的情况。

## 为什么需要 transaction journal

单个 node 是原子页写，但 split/merge/root change 涉及多页。例如 C10 split 为 C10/C15，且 P10 增加链接：

| 崩溃时已落盘内容 | 后果 |
|---|---|
| 仅 C15 | 不可达的新节点，旧树仍完整 |
| 仅 C10 | 数据可能已从 C10 移出、C15 又不可达，存在数据丢失 |
| 仅 P10 | parent 指向未写入 node，结构损坏 |
| C15 + C10，P10 未写 | 数据存在，但 parent 需要恢复修复 |

因此 checkpoint 前需将结构变更的最小描述持久化到 metadata journal。设计详情见 `src/lib/index/README.md:9-43`。

`txn_journal` 和 `txn_record` 位于 `index_cp.hpp:35-136`。一个记录保存：

```text
index ordinal
可选 inplace parent
可选 inplace child
新建 node IDs
释放 node IDs
父节点是否为 metadata buffer
释放节点层级
```

记录顺序为：

```text
parent_inplace → child_inplace → child_new[] → child_freed[]
```

它仅保存 block/chunk ID，不复制完整 node payload；恢复只需据此判断哪些 node 已提交并重建关系。

## BTree split/merge 如何生成 journal 与 DAG

`IndexTable::transact_nodes` 先标脏参与的 node，再调用：

```cpp
wb_cache().transact_bufs(ordinal(), parent_buf, child_buf,
                         new_node_bufs, freed_node_bufs, cp_ctx);
```

见 `index_table.hpp:424-447`。

`IndexWBCache::transact_bufs` 负责：

```text
建立 parent/child/new/freed 的 DAG 关系
→ 记录 transaction journal
```

见 `wb_cache.cpp:281-361`。

`link_buf(up, down)` 令 `down` 先于 `up` flush。对于同一 CP 内的新节点到新节点的关系，代码会进行 flatten，避免形成不必要深链；对于 sibling link，允许按 merge/rebalance 的要求重新绑定依赖。具体复杂规则和示例注释见 `wb_cache.cpp:363-445`。

## Checkpoint flush

`IndexCPCallbacks` 注册为 `cp_consumer_t::INDEX_SVC` 的 consumer，见 `index_cp.cpp:10-23`。

一个 CP 的主要流程：

```text
BTree 修改节点
→ write_node_impl 将 node 标为 DIRTY
→ 加入本 CP dirty list
→ transact_bufs 更新 DAG 和 journal
→ CPManager 发起 cp_flush
→ 先持久化 journal
→ 按 DAG 从 child/new node 到 parent/meta node flush
→ 更新已修改表的 superblock
→ VDev cp_flush 持久化 allocator/free-list 等 CP 信息
→ CP complete
```

### Dirty 标记

`write_node_impl` 将 buffer 状态置为 `DIRTY`；首次变 dirty 时调用 `write_buf`，后者将 buffer 加入 `IndexCPContext::m_dirty_buf_list`，并递增 ResourceMgr dirty buffer 计数。见 `index_table.hpp:396-421`、`wb_cache.cpp:127-143`。

### Journal 必须先写

`async_cp_flush` 首先更新或创建 `wb_cache` metadata sub-superblock：

```cpp
meta_service().update_sub_sb(journal_buf.cbytes(), journal_buf.size(), m_meta_blk);
```

见 `wb_cache.cpp:973-984`。这是 write-ahead 规则：即使后续 node 写到一半崩溃，恢复程序仍拥有完整结构变更描述。

### 并行 flush

WBCache 创建多个 `index_cp_flush<N>` reactor，数目由 `generic.cache_flush_threads` 决定，见 `wb_cache.cpp:65-93`。

可 flush 的条件是：

```cpp
state == DIRTY
&& dirtied_cp_id == current_cp
&& wait_for_down_buffers == 0
```

见 `wb_cache.cpp:1170-1207`。

节点完成后递减上游 buffer 的依赖数；若上游依赖变为 0，则可继续 flush。无依赖的不同 DAG 分支能被多个 flush fiber 并行处理。

### 完成 CP

最后一个 dirty buffer 完成时：

```text
更新所有涉及 table ordinal 的 table superblock
→ 在 blocking IO fiber 调用 m_vdev->cp_flush(cp_ctx)
→ 持久化 allocator bitmap 与 free-list
→ cp_ctx->complete(true)
```

实现见 `wb_cache.cpp:1096-1129`。

## Root change 的特殊处理

table superblock 的 `root_node` 是持久化入口，因此 root split/collapse 需要特别处理。

### Root split

root split 中可能同时发生：旧 root 原地修改、新 root 创建、superblock 指向新 root。若 metadata 提前落盘而新 root 未写，重启会指向无效 root。

`IndexCPContext` 识别 root change，见 `index_cp.cpp:28-68`。checkpoint flush 会先获取 root transition 所需的 preflush buffers：

```cpp
auto preflush_bufs = cp_ctx->root_change_preflush_bufs();
```

并在正常 DAG flush 前先物理写这些节点，见 `wb_cache.cpp:986-1035`。

### Root collapse

root collapse 可能使旧 root 标记 deleted，唯一 child 成为新 root，而 crash 发生在 superblock 更新前。journal 会记录最终 root candidate；恢复时从 journal 和已提交 node 中找出候选，调用：

```cpp
table->set_root_from_committed_buf(candidate);
```

该方法验证 node、更新 `root_node/root_link_version/btree_depth`、写 superblock，并更新内存 root，见 `index_table.hpp:273-322`。

## 恢复流程

`IndexService::start` 的顺序（`index_service.cpp:95-133`）：

```text
创建 IndexWBCache，并注册 CP consumer
→ 加载所有 table superblock
→ callback 构造具体 IndexTable<K,V>
→ 恢复 ordinal 和 table 映射
→ WBCache::recover 恢复未完成 CP
→ 对每张表调用 recovery_completed
→ 强制一个 recovery checkpoint
```

先恢复表、后恢复 WBCache 是必要的：journal 中只含 ordinal，修复过程必须能通过 ordinal 找到正确的 `IndexTableBase`。

### 无需恢复的情况

若 `wb_cache` journal 不存在或为空，WBCache 仅调用 `m_vdev->recovery_completed()`，见 `wb_cache.cpp:554-559`。

### 从 journal 重建 DAG

`IndexCPContext::recover` 解析 transaction records，创建 IndexBuffer 占位、恢复 parent/child/new/freed 关系，重建 `m_up_buffer` 与 `m_wait_for_down_buffers`，见 `index_cp.cpp:256-351`。

MetaIndexBuffer 的物理 BlkId 为空，多张表同 CP root 变更时会冲突；实现以：

```cpp
BlkId{index_ordinal, 0, 0}
```

作为仅用于恢复 map 的 synthetic key，见 `index_cp.cpp:357-406`。

### 两阶段恢复

`IndexWBCache::recover` 明确分两阶段，见 `wb_cache.cpp:595-605`：

1. **先处理 new/freed node**：恢复 allocator 占用、确定应丢弃的新节点、标记和回收被删除节点，避免修复过程中 block ID 被错误复用。
2. **再修复树结构**：删除 stale child links、修复 parent/sibling links、修复未提交的上游 node、提升 committed root candidate、最后释放确认删除的 blocks。

恢复判断 node 是否已提交时，会读取物理页，检查 BTree node 格式、node modified CP ID 等，逻辑位于 `wb_cache.cpp:920-933`。

## 节点和结构修复

`IndexTable::repair_node`：验证 buffer 和 node ID，将 raw page 包装为 BTreeNode；对 interior node 调 `repair_links`；若其上游为 metadata buffer，则更新 root，见 `index_table.hpp:340-384`。

`delete_stale_children`：遍历 parent links，移除指向 `node_deleted` child 的 link，修正 edge/前驱 child，必要时删除空 parent，并将修改纳入当前 CP，见 `index_table.hpp:516-612`。

`repair_links` 是最复杂的恢复例程，见 `index_table.hpp:667-1086`。它通过 child 的 sibling chain 重建 parent entries、跳过 deleted child、修正 next link/edge/最后 key；必要时拆分修复中的 parent 并处理 root 变化。

## MetaIndexBuffer

每张表都有 `MetaIndexBuffer`，它将 table superblock 作为 DAG 内节点处理：

```text
new child/root node → root node → MetaIndexBuffer / table superblock
```

root 变更时先更新内存 superblock，再 `refresh_meta_buf` 创建或刷新 metadata 的 CP buffer，并通过 `transact_bufs` 把 metadata buffer 依赖在 root buffer 上，见 `index_table.hpp:475-505`、`wb_cache.cpp:200-220`。

这样 metadata 不会绕过节点持久化顺序提前写出。

## 表销毁

`IndexTable::destroy`（`index_table.hpp:135-154`）执行：

```text
销毁 BTree / 标记释放 blocks
→ 强制并等待当前 checkpoint 完成
→ 删除 table superblock
→ 使 MetaIndexBuffer 失效
```

强制 CP 先于删除 metadata，避免旧 journal 在崩溃后 replay 时引用已不存在的 table ordinal。

## 生命周期

HomeStore 的顺序是：

```text
MetaService start
→ CPManager start
→ IndexService start
→ 其他服务 start
```

见 `src/lib/homestore.cpp:276-315`。

关闭时：

```text
停止上层服务
→ IndexService stop（拒绝新 table 请求）
→ CPManager shutdown，执行最终 checkpoint
→ 销毁 IndexService
→ 停止 metadata 和设备
```

见 `homestore.cpp:318-374`。

IndexService 自身 stop 只等待服务请求、停止表；最终持久化由 CPManager shutdown 完成。

## 并发与可观测性

- `IndexTable` 使用 stopping flag 和请求计数拒绝停机后请求；`put/remove` 在 CP mismatch 时重试。
- cache 仅逐出 clean node，dirty/flushing node 不会被安全驱逐。
- 多个 checkpoint flush fiber 并行写不同 DAG branch；`m_flush_mtx` 保护调度和状态迁移。
- `IndexService` 的 UUID/ordinal map 使用单一 mutex；当前查表也采用独占锁，主要影响恢复、表管理和 metadata 更新路径。
- `IndexTable::audit_tree`、`sanity_check` 用于恢复后的结构验证。

## 测试覆盖

- `test_index_btree.cpp`：顺序/随机读写、range update、cache eviction、单/多 CP、并发 CP、root collapse。
- `test_index_crash_recovery.cpp`：在 split、merge、rebalance、root、metadata 的不同写入阶段注入 crash，验证恢复可继续工作。
- `test_index_gc.cpp`：基于 chunk key 的并发范围删除、GC、读写与 allocator 回收。

## 当前实现注意项

1. **不是同步事务性索引**：普通 put/remove 成功并不等于已落盘；业务强 durability 需要上层日志和重放。
2. **恢复路径偏正确性而非性能**：会读取、验证和重建多个 parent/child/sibling node，符合 README 的设计取舍。
3. **恢复依赖业务 callback**：业务必须提供正确 `IndexServiceCallbacks::on_index_table_found`，并按正确模板和 BTreeConfig 构造表。
4. **`index_size` 需进一步核实**：`index_table_sb` 有该字段，`IndexTable::used_size()` 直接返回它，但当前 IndexService/IndexTable/WBCache 主路径中未见明显的维护代码；若用于容量统计，需继续确认 BTree 基类或上层是否更新。
5. **恢复失败倾向于 assert/故障隔离**：一些复杂边界使用断言，而不是将单表 offline 后继续服务。
6. **修复代码仍有 TODO**：例如 `repair_links` 对某些会否导致更高层结构增长的情况保留 TODO，应视为复杂故障路径的后续演进点。

## 总结

IndexService 是建立在固定原子页、VirtualDev 块分配器与 CPManager 之上的通用持久化 B+Tree 服务：

1. 用 `IndexTable<K,V>` 将 BTree 算法与 HomeStore 存储连接；
2. 用 WBCache 延迟和合并 node 写入；
3. 用 checkpoint journal 记录 split/merge/root 的最小恢复信息；
4. 用 child→parent 的依赖 DAG 控制安全落盘次序；
5. 用恢复 DAG、页校验、parent/root 修复和 allocator 修复处理未完成 checkpoint；
6. 保证此前持久化数据与 BTree 结构安全，而把尚未 checkpoint 的业务数据 durable 责任交给上层 journal。
