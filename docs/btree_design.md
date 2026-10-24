# HomeStore B+Tree 设计与实现详解

HomeStore 的 Btree 实现采用了经典的**分层设计**：一个与存储介质无关的通用 B+Tree 算法模板（`Btree<K,V>`），下接一个可插拔的节点物理格式层（`BtreeNode` 及其多种子类实现），再通过虚函数钩子对接到具体的存储后端——持久化场景对接 `IndexTable`（进而对接 `IndexWBCache`/CP），纯内存场景对接 `MemBtree`。

```
应用层 (K/V 类型定义)
        │
Btree<K,V>  (btree.hpp / btree.ipp / detail/*.ipp)  — 平台无关的 B+Tree 算法：put/get/remove/query/split/merge/锁协议
        │  (纯虚函数: alloc_node/read_node_impl/write_node_impl/refresh_node/free_node_impl/transact_nodes/on_root_changed)
        ├── IndexTable<K,V>  (index_table.hpp)  — 持久化实现，对接 IndexWBCache + CP + meta superblock，并实现崩溃修复算法
        └── MemBtree<K,V>    (mem_btree.hpp)    — 纯内存实现，直接 new/delete 节点，无持久化

BtreeNode (抽象，detail/btree_node.hpp)
        │ (纯虚函数: insert/remove/update/get_nth_xxx/move_out_to_right_by_size/copy_by_size ...)
        ├── VariantNode<K,V>       (detail/variant_node.hpp)  — 实现 multi_get/multi_put/multi_remove/get_any 等通用算法
        │     ├── SimpleNode<K,V>        (simple_node.hpp)   — 定长 K/V，FIXED 布局
        │     ├── VariableNode<K,V>      (varlen_node.hpp)   — 变长 K/V 基类，offset+record 布局
        │     │     ├── VarKeySizeNode / VarValueSizeNode / VarObjSizeNode
        │     └── FixedPrefixNode<K,V>   (prefix_node.hpp)   — 针对"区间型"K/V 的前缀压缩节点
```

下面逐层展开。

---

## 一、Btree 算法层的核心抽象

### 1.1 BtreeConfig (`btree_internal.hpp:240-297`)

每棵树的行为参数集中放在 `BtreeConfig`：
- `m_node_size`/`m_node_data_size`：节点总大小（= vdev 原子页大小，见 IndexService 设计）与除去 header 后的可用数据区大小
- `m_ideal_fill_pct`(90%)/`m_suggested_min_pct`(30%)/`m_split_pct`(50%)：分别用于**合并阈值判定**（低于 suggested_min_size 触发合并候选）、**分裂切分比例**（分裂时从旧节点移出约 50% 的填充量到新节点）
- `m_max_merge_nodes`(默认 3)：一次 remove 操作最多尝试合并多少个兄弟节点
- `m_max_merge_level`(默认 1)：只有 ≤ 该层级的节点才参与合并（通常只合并叶子，减少对上层结构频繁调整的代价）
- `m_leaf_node_type`/`m_int_node_type`：叶子节点和内部节点可以**各自选择不同的物理格式**（如叶子用 `VAR_OBJECT`，内部节点用 `VAR_KEY`，因为内部节点的 Value 固定是 `BtreeLinkInfo` 定长的）
- `_PRERELEASE` 下还有 `m_max_keys_in_node`/`m_min_keys_in_node`，用于测试时人为制造频繁分裂/合并来压测崩溃恢复路径

### 1.2 Key/Value 抽象 (`btree_kv.hpp`)

- `BtreeKey`/`BtreeValue`：纯虚接口，要求实现 `compare`/`serialize`/`deserialize`/`to_string`
- `BtreeIntervalKey`/`BtreeIntervalValue`：**区间型**键值的扩展接口，专门为 `FixedPrefixNode` 前缀压缩设计，要求支持：
  - `shift(n, ctx)`：把键"向后移动 n"（典型场景是连续 LBA/offset 这类可枚举的区间键）
  - `distance(from)`：两个 key 之间的距离
  - `serialize_prefix()/serialize_suffix()`：把一个 key 拆成"公共前缀部分"和"变化的后缀部分"分别序列化——这是前缀压缩的基础
- `BtreeKeyRange<K>`：范围查询/range put/range remove 的统一描述，带 `m_start_incl`/`m_end_incl`/`MultiMatchOption`（DO_NOT_CARE/LEFT_MOST/RIGHT_MOST/MID，用于 `get_any` 这种模糊匹配场景）
- `BtreeTraversalState<K>`：范围操作过程中维护的**滑动窗口状态**——`m_input_range`（用户原始请求范围）与 `m_working_range`（当前递归层实际还需处理的子范围）。`trim_working_range`/`shift_working_range` 实现了范围操作在多层节点间下钻、以及跨多次 batch 调用（分页查询）时的状态推进。
- `BtreeLinkInfo` (`btree_kv.hpp:244-290`)：**内部节点里 Value 的实际类型**，本质是 `{bnodeid_t, link_version}` 的二元组而非直接存储子节点数据——每条父子边都带一个 **link_version**，这是实现崩溃恢复中父子一致性校验的关键字段（见后文 `repair_links`）。

### 1.3 节点持久化头 `persistent_hdr_t` (`detail/btree_node.hpp:49-93`)

这是每个节点物理 buffer 最前面的定长 pack(1) 结构，总大小 72 字节左右：

```cpp
uint8_t magic, version; uint16_t checksum;
uint32_t nentries:30, leaf:1, node_deleted:1;
bnodeid_t node_id, next_node;      // next_node: 同层右兄弟链接 (叶子层形成单向链表，用于 sweep 查询)
uint64_t node_gen;                 // 每次修改自增 —— 乐观锁/CAS 的核心
uint64_t link_version;             // 与父节点中记录的 link_version 配对，用于检测父子链接是否过期
BtreeLinkInfo::bnode_link_info edge_info;  // "edge" 指向最右侧溢出子节点(没有对应 key 的子指针)
int64_t modified_cp_id;            // 最近一次修改发生在哪个 CP —— 和 IndexService 的 IndexBuffer::m_dirtied_cp_id 呼应
uint16_t level; uint16_t node_size; uint8_t node_type;
```

`checksum` 用 `crc16_t10dif` 对 header 之后的数据区计算，`is_valid_node()` 在读回/恢复时校验 magic+version+size+checksum 四件套，这是崩溃恢复判断"这个块是否曾经被完整写入过"的依据（即 `was_node_committed`）。

`transient_hdr_t`（`detail/btree_node.hpp:33-44`）是**纯内存态**、不持久化的部分：一把 `shared_mutex`（节点锁）+ `upgraders` 计数器（用于检测"有没有人在等锁升级，避免死锁"）。

### 1.4 BtreeNode 抽象基类职责 (`detail/btree_node.hpp:96-577`)

`BtreeNode` 不关心具体物理布局，但实现了所有**与布局无关的通用逻辑**：
- `find(key, ...)`/`bsearch_node`/`bsearch`：标准二分查找，返回 `(found, idx)`；对内部节点，`idx == total_entries()` 且 `has_valid_edge()` 表示命中最右侧 edge 指针
- `match_range(range, start_idx, end_idx)`：把一个 `BtreeKeyRange` 映射成该节点里的 `[start_idx, end_idx]` 闭区间——对内部节点和叶子节点处理语义略有不同（叶子节点需要精确排除不在范围内的边界条目；内部节点只需覆盖所有可能相交的子节点范围）
- `insert/remove_one/remove_any/update_one`：调用 `find` 后转发给纯虚的单点 `insert(idx,...)`/`remove(idx)`
- `node_gen()`/`inc_gen()`：**乐观并发控制的核心**——每次结构性修改（insert/update/remove/move）都会 `inc_gen()`，上层通过比较"拿锁前后 gen 是否变化"来判断节点有没有在无锁窗口期被并发修改过（见 1.5 节）
- `is_merge_needed(cfg)`：`occupied_size() < suggested_min_size()` （或测试模式下 `total_entries() < min_keys_in_node`）
- 持久化相关 getter/setter 全部是对 `persistent_hdr_t` 字段的直接读写（`node_id()`/`set_checksum()`/`set_modified_cp_id()`/`has_valid_edge()` 等）

纯虚函数（留给布局子类）包括：`insert(ind,key,val)`、`remove(ind_s,ind_e)`、`update`、`move_out_to_right_by_size/entries`、`copy_by_size/entries`、`available_size`、`has_room_for_put`、`get_nth_key_internal`、`get_nth_value` 等——这组接口是分裂/合并算法赖以工作的"节点内容搬运原语"。

### 1.5 节点锁协议：乐观读优先 + 代际号校验

这是整个 Btree 并发设计里最值得展开的部分。核心矛盾：写操作（insert/split/merge）需要写锁，但大多数时候遍历内部节点只是"读"，如果每层都直接上写锁，并发度会很差。HomeStore 采用的策略：

1. **自顶向下先尝试读锁**：`put`/`remove` 一开始都用 `locktype_t::READ` 锁住 root（`btree.ipp:88-91`）。
2. **叶子层强制写锁**：当发现目标是叶子节点时，若当前锁不是 WRITE，就解锁重试并直接申请写锁（`btree.ipp:122-126`）——因为叶子一定要改。
3. **内部节点锁升级（lock-coupling + upgrade）**：当某个内部节点的子节点判断需要分裂/合并时，才调用 `upgrade_node_locks`（`btree_node_mgr.ipp:122-190`）把父子节点的读锁**原子地升级**为写锁。升级过程：先解锁父子两个节点 → 重新按 WRITE 加锁父节点 → 再加锁子节点 → 比较升级前后记录的 `node_gen()`：**如果 gen 变了（说明升级的间隙里别的线程已经修改了该节点结构，比如被其他线程合并/分裂过），直接返回 `btree_status_t::retry`**，上层会从 root 重新开始整个操作（`btree.ipp:129-136` 的 `goto retry`）。
4. **向下传递时及时释放父锁**：`do_put`/`do_remove` 在遍历到某个子节点的最后一个匹配索引时，会提前 `unlock_lambda(my_node, curlock)` 释放父节点锁再递归下钻（`btree_mutate_impl.ipp:153-157`），把锁持有时间降到最低——这是一种**蟹行协议（crab-walking / lock coupling）**的变体，只不过先尝试读锁，必要时才升级。
5. **节点被删除检测**：无论是 `upgrade_node_lock` 还是 `upgrade_node_locks`，升级成功后都会检查 `node->is_node_deleted()`，如果节点在等锁期间被别的线程标记删除（典型发生在被合并掉），同样返回 `retry`。

整个设计本质上是 **optimistic concurrency control + 乐观失败重试（retry-from-root）**，而不是传统 B-link 树那种"永远持有父锁直到完成"的悲观协议，目的是让绝大多数不涉及分裂/合并的 put/get 操作只需要读锁或叶子写锁，最大化并发度。

此外内部节点还维护了一条 `next_bnode` 同层链表（B-link 风格），这让 sweep 查询可以在不回溯父节点的情况下沿兄弟链表连续扫描叶子（见 1.9 节查询部分），也是修复算法重建父子链接时用来"摸排"兄弟节点的依据。

---

## 二、写路径：`put` 与节点分裂

### 2.1 `Btree::put` 总体流程 (`btree.ipp:84-150`)

1. 读锁住 root；若 `is_split_needed(root)` 为真，先解锁，调用 `check_split_root` **单独加全树写锁**（`m_btree_lock.lock()`）做 root 分裂，然后从头 `retry`。
2. 若 root 是叶子但当前只有读锁，解锁升级成写锁重试。
3. 否则调用 `do_put` 递归下钻；若返回 `retry`（锁升级失败/root 分裂）或 `has_more`（range-put 未写完一批）则从 root 重新开始。

### 2.2 `do_put` 递归下钻 (`btree_mutate_impl.ipp:32-169`)

- 叶子节点：直接 `mutate_write_leaf_node` 完成插入/更新并 `write_node`。
- 内部节点：
  - 用 `find`（单点 put）或 `match_range`（range put）定位 `[start_idx, end_idx]` 要下钻的子节点区间；
  - 对每个子节点，如果 `is_split_needed(child)`，先 `upgrade_node_locks` 升级父子锁为 WRITE，调用 `split_node`，然后 `goto retry` **从当前节点重新开始搜索**（因为分裂后父节点内容变了，idx 需要重新定位）；
  - 否则递归调用 `do_put(child, ...)`；
  - 支持 range put 时维护 `BtreeTraversalState` 的 `working_range` 随着下钻不断裁剪（`trim_working_range`）。

### 2.3 `split_node` (`btree_mutate_impl.ipp:255-304`)

1. 新分配一个同类型（leaf/interior）节点 `child_node2`；
2. 维护叶子层链表：`child_node2->next = child_node1->next; child_node1->next = child_node2`；
3. 按 `m_bt_cfg.split_size(filled_size)`（默认填充量的 50%）调用 `move_out_to_right_by_size` 把约一半内容从旧节点搬到新节点；
4. 取旧节点（现在是"左节点"）的最后一个 key 作为 `split_key`；
5. **先更新父节点中旧条目指向 `child_node2`，再插入新条目 `(split_key, child_node1)`**——代码注释特别强调顺序不能反，否则如果 split_key 恰好是父节点的最后一个 key（即在更新 edge 的场景），插入顺序错误会导致边界条目丢失；
6. `child_node1->inc_link_version()`：父子链接版本号自增，为崩溃恢复中区分"这是不是同一代链接"做标记；
7. 调用 `transact_nodes({child_node2}, {}, child_node1, parent_node, context)` 交给存储后端去做**持久化事务**（即 `IndexWBCache::transact_bufs`，建立依赖 DAG + 写 journal）。

Root 分裂 (`check_split_root`, `btree_mutate_impl.ipp:197-252`) 的特殊之处：先分配新 root 节点、**先调用 `on_root_changed` 通知持久层根变更**（这样依赖关系在分裂动作发生前就已建立，保证恢复时能正确识别"这是一次 root split"），再把旧 root 作为子节点传给 `split_node`，整个过程持有全树写锁 `m_btree_lock`，避免和并发的 root 访问冲突。

`is_split_needed`（`btree_mutate_impl.ipp:306-318`）判定依据是 `!node->has_room_for_put(...)`：内部节点按"最大 key + 定长 LinkInfo"估算是否还能再放一条，叶子节点则按实际要插入的 key/value 大小判断。

---

## 三、删除路径：`remove` 与节点合并

### 3.1 整体流程 (`btree.ipp:176-235`)

和 put 对称：root 为空且非叶子时先解锁触发 `check_collapse_root`（收缩树高）；root 是叶子但只有读锁则升级重试；否则 `do_remove` 递归。

### 3.2 `do_remove` (`btree_remove_impl.ipp:22-176`)

叶子节点直接删除对应 key。内部节点下钻前先判断 `child_node->is_merge_needed(cfg)`：若需要合并，计算一个合并窗口 `[curr_idx, node_end_idx]`（最多 `m_max_merge_nodes` 个兄弟，且不超过父节点范围），升级锁后调用 `merge_nodes`；合并成功后 `goto retry`（父节点内容变了需要重新定位），合并失败（`merge_not_required`）则继续正常下钻。

### 3.3 `check_collapse_root` (`btree_remove_impl.ipp:178-240`)

当 root 没有条目只剩一个 edge 子节点时，把这个唯一子节点提升为新 root（树高减一）。这里有一段非常值得注意的注释和代码顺序：

```cpp
// CRITICAL: Write the child node BEFORE calling on_root_changed()
write_node(child, req.m_op_context);
ret = on_root_changed(child, req.m_op_context);
```

注释解释了两个潜在的 CP 死锁场景：
1. 如果 child 是 CLEAN（上一个 CP 创建、本次未修改），`on_root_changed` 内部会通过 `link_buf` 把它设为 meta buffer 的依赖，但 CLEAN buffer 永远不会进入 CP 的脏列表去触发 flush，导致 meta 的等待计数永远减不到 0，**CP 死锁**。
2. 即使 child 本来是脏的，如果在"建立依赖关系"和"加入脏列表"之间发生了 CP 切换，child 可能落到下一个 CP 的脏列表里，而 meta 却在当前 CP 等它，形成跨 CP 依赖死锁。

解决办法很直接：**先 `write_node(child)` 确保它进入当前 CP 脏列表，再建立依赖关系**。这是一处由实际 CP 挂死问题驱动、代码和注释都明确记录下来的设计决策。

### 3.4 `merge_nodes` —— 整个 Btree 实现里最复杂的函数 (`btree_remove_impl.ipp:243-643`)

这个函数要同时处理**合并**（多个节点并成更少的节点）和**再平衡/rebalance**（节点数不变但重新均分内容）两种情况，核心算法：

1. **决定要不要真正减少节点数**：先累计 `total_size`/`total_entries`（leftmost + 所有待合并兄弟），按 `ideal_fill_size()` 算出理论上需要几个节点 `num_nodes`。
   - 若 `num_nodes >= old_nodes.size()+1`，说明装不下更少节点，只能做 rebalance；若 `m_rebalance_turned_on` 为 false 就直接放弃（返回 `merge_not_required`）。
2. **按 `balanced_size` 均分重新切割**：对 `leftmost_node` 做"干跑"（dry-run）计算，优先往 leftmost 里多塞（因为它是唯一原地复用、不需要新分配节点的），再决定后续的内容应该切几个新节点来装（`leftmost_src`/`src_cursor` 两个辅助结构记录切割游标）。
3. **真正执行拷贝**（"this is where no going back" 的注释标明了"point of no return"）：`leftmost_node->copy_by_entries(...)` 把切好的部分拷入 leftmost，再为剩余内容分配 `new_nodes` 并逐个 `copy_by_size` 填充。
4. **更新父节点索引**：先删除多余的旧子节点条目 (`parent_node->remove(start_idx+1, start_idx+excess)`)，再从右往左把 `new_nodes` 的最后 key 更新进父节点，最后用 leftmost 的新内容更新父节点起始索引项，并维护 `next_bnode` 链表的重新串接。
5. 大量 `#ifndef NDEBUG` 断言校验（父子 key 排序、link_version 一致性等）保障合并后结构不变量。
6. 失败路径：不管在哪一步失败，`out:` 标签统一按"倒序解锁旧节点、释放新分配节点"做回滚——因为旧节点此时还没被真正修改（都是先算后写），新节点只是分配出来还没持久化挂接，可以安全丢弃。
7. 成功路径同样调用 `transact_nodes(new_nodes, old_nodes /* freed */, leftmost_node, parent_node, context)` 交给持久层建立依赖 DAG 和 journal。

`FixedPrefixNode` 的合并计算还要特别考虑**前缀槽位**（`expected_holes`/`expected_tail`）的变化，因为前缀压缩节点的"空间占用"不是简单的线性关系（多个 suffix 可能共享同一个 prefix slot）。

---

## 四、读路径

### 4.1 单点 `get` (`btree_get_impl.ipp`)

最简单的一条路径：全程只用读锁，自顶向下 `find`，定位到叶子后直接返回，不做任何写操作或 split/merge 判断。

### 4.2 范围查询 `query` 的两种模式 (`btree_query_impl.ipp`)

- **SWEEP_NON_INTRUSIVE_PAGINATION_QUERY**（`do_sweep_query`）：先下钻到起始 key 所在叶子（下钻过程中逐层释放父锁，不保留路径），命中后**沿叶子节点的 `next_bnode()` 链表横向扫描**，不需要每次都回到 root 重新查找兄弟节点，效率最高，适合大范围顺序扫描。但由于不持有跨调用的锁，两次分页调用之间树可能已经变化（"非侵入式"分页，游标用最后一个 key 重新定位，下次调用重新从 root 搜索）。
- **TREE_TRAVERSAL_QUERY**（`do_traversal_query`）：每次都严格按树形结构从 root 递归下钻找到范围覆盖的所有子节点分支，不依赖 `next_bnode` 兄弟链接。效率不如 sweep，但在父子节点内容出现代际不一致（典型是崩溃恢复后的中间状态）时更健壮，所以主要用于恢复校验场景。
- 分页游标：`BtreeTraversalState::shift_working_range()` 在每次返回后，把上次最后一个 key 设为下次查询的新起点（`false` 表示不包含该 key）。

### 4.3 节点内的 `multi_get`/`get_any` (`variant_node.hpp`)

`multi_get` 利用 `match_range` 拿到 `[start_idx,end_idx]` 后批量取出并应用 `filter_cb` 过滤；`get_any` 支持 `MultiMatchOption`（LEFT_MOST/RIGHT_MOST/MID/DO_NOT_CARE），是给"只需要范围内任意一条匹配"的场景用的（比如查找某个 range 内是否存在空闲块）。

---

## 五、节点物理格式的三种实现

### 5.1 SimpleNode / FIXED 布局 (`simple_node.hpp`)

最简单的定长记录布局：`[Header][Obj0][Obj1]...[ObjN]`，每个 Obj 固定大小 `= key_size + value_size`（通过 `dummy_key<K>.serialized_size()` 静态获取，要求 K/V 都是定长类型）。插入/删除都是直接 `memmove` 搬移整块定长记录，没有任何碎片管理，`available_size()` 直接按剩余条目数计算。最简单也最快，但要求定长 K/V——通常用于内部节点（`BtreeLinkInfo` 恰好定长）。

### 5.2 VariableNode 及其三种变体 (`varlen_node.hpp`)

布局：`[Header][var_node_header][Record0][Record1]...   ......   [KV_N][...][KV_1][KV_0]`——**记录区从前往后增长，数据区（key+value 实际字节）从后往前增长**，两者向中间靠拢。每条 `Record` 只存一个 `m_obj_offset`（指向数据区位置）+ 可选的变长字段：
- `VarKeySizeNode`：key 变长，value 定长（record 里存 `m_key_len`）
- `VarValueSizeNode`：key 定长，value 变长（record 里存 `m_value_len`）
- `VarObjSizeNode`：key、value 都变长（record 里同时存两个长度）

**碎片整理（compact）**：删除/更新会在数据区留下空洞（`m_available_space` 增加但 `tail_arena_offset` 不变，即整体空间够但不连续）。`insert` 时如果 `to_insert_size > get_arena_free_space()`（尾部连续空闲不够但总可用空间够），触发 `compact()`：按 `m_obj_offset` 降序排序所有记录，从尾部往前依次挪紧，消除所有空洞，重新收紧 `tail_arena_offset`。这是一个经典的**变长堆内压缩**算法。

`move_out_to_right_by_size/entries`：从本节点末尾（即数据区域的头部，entries 按 key 序排列）逐条取出 blob 插入对端节点的索引 0（即逐条向右移动，插入对端最前面来保证顺序），这个实现有一处精细处理：中途用 `this_gen`/`other_gen` 暂存原始 gen，操作完后手动 `set_gen(gen+1)`，避免因为内部调用了多次 insert/remove 而让 `node_gen()` 虚高（这会导致乐观锁误判有并发修改发生，引发不必要的 retry）。

### 5.3 FixedPrefixNode —— 前缀压缩节点 (`prefix_node.hpp`)

这是专为**区间型 Key/Value**（`BtreeIntervalKey`/`BtreeIntervalValue`，典型场景是基于连续 LBA/offset 的范围索引，如 extent map）设计的高压缩比布局：

```
[Header][prefix_node_header][prefix_bitset][... suffix_entry 区，从头往后增长 ...]
                                                                [... prefix_entry 区，从尾往前增长 ...]
```

核心思想：很多相邻的 key/value 共享相同的"公共前缀"（比如一串连续分配的 LBA 对应同一个物理 extent 的前缀部分），所以把每条记录拆成：
- **`prefix_entry`**：存一份"公共前缀"的 key/value 片段 + `ref_count`（有多少条 suffix 在引用它）
- **`suffix_entry`**：只存"变化的后缀"部分 + 一个指向 `prefix_entry` 槽位的 `prefix_slot` 索引

`prefix_bitset_`（`sisl::CompactBitSet`）管理 prefix 槽位的分配/回收，`alloc_prefix`/`deref_remove_prefix`（引用计数归零才真正释放槽位并清位）、`compact()`（把散落在后部槽位区的"空洞"向前紧缩，类似前面变长节点的 compact，但作用对象是**槽位数组**而不是字节流）。

这个格式对写路径（`multi_put`/`multi_remove`）做了专门重写而不是复用 `VariantNode` 默认实现，因为前缀压缩天然适合**区间批量写入**：`multi_put` 利用 `BtreeIntervalKey::shift(1, ctx)` 在一个循环里把 range 内每个 key "一格一格"地推进，并通过比较"新值是否仍能复用当前 prefix_slot"（`get_prefix_entry_c(prefix_slot)->compare(cur_key, new_val)`）来决定是复用还是开一个新的 prefix slot——这正是**自适应前缀分组**的核心逻辑。

合并/拷贝（`copy_internal`/`move_out_to_right_internal`）都要维护一个 `src_prefix_slot -> my_prefix_slot` 的映射表，保证同一个 prefix 在跨节点搬运时只拷贝一份、引用计数正确累加，避免重复存储。

---

## 六、IndexTable：把通用 Btree 接到持久化与崩溃恢复

`IndexTable<K,V>` 同时继承 `IndexTableBase`（给 `IndexService` 用的类型擦除接口）和 `Btree<K,V>`（获得通用算法），它要实现 `Btree` 留下的 7 个纯虚钩子，把抽象的节点操作映射成真实的 `IndexWBCache` 调用：

| Btree 钩子 | IndexTable 实现要点 |
|---|---|
| `alloc_node` | 调 `wb_cache().alloc_buf(ordinal, initializer)`，拿到 `IndexBuffer` 后用 `init_node` 按配置的节点类型构造出具体子类对象，`attach_buf` 绑定 |
| `read_node_impl` | 调 `wb_cache().read_buf`，命中内存 cache 直接返回，否则从 vdev 同步读盘构造节点 |
| `write_node_impl` | 把节点状态从 CLEAN 切到 DIRTY，`set_checksum()`，**第一次变脏**时才调用 `wb_cache().write_buf` 把它加入 CP 脏列表 |
| `refresh_node` | 只有在 `for_read_modify_write` 时才调用 `wb_cache().get_writable_buf`——即"是否需要 CoW 拷贝一份新 buffer"的判断（`m_dirtied_cp_id` 是否等于当前 CP） |
| `free_node_impl` | 调 `wb_cache().free_buf` |
| `transact_nodes` | 把 split/merge 产生的 new_nodes/freed_nodes 都先各自 `write_node_impl` 落到脏列表，再统一调 `wb_cache().transact_bufs` 建立依赖 DAG + 写 journal |
| `on_root_changed` | 更新 `m_sb`（superblock）里的 root_node/link_version/btree_depth/节点计数，调 `wb_cache().refresh_meta_buf` 拿到/刷新 MetaIndexBuffer，再 `transact_bufs` 把新 root 挂到 meta buffer 下面 |

### 6.1 put/remove/get 的 CP 包装与重试

`IndexTable::put/remove/get`（`index_table.hpp:163-206`）在真正调用 `Btree<K,V>::put/remove/get` 之前，先用 `cp_mgr().cp_guard()` 拿到当前 CP 上下文并塞进 `req.m_op_context`。如果 `Btree` 返回 `btree_status_t::cp_mismatch`（典型发生在 `get_writable_buf` 判断"要写的是上一个 CP 的 buffer，但又不是当前 CP 或上一个 CP"的异常情形），就在**外层**用 `do { ... } while (ret == cp_mismatch)` 重新拿一次 cp_guard 重试，这保证了 CP 切换瞬间的竞态不会导致操作失败，只是多重试一次。

同时还叠加了和 `IndexService` 一致的"优雅停机"模式：`is_stopping()`/`incr_pending_request_num()`/`decr_pending_request_num()`，确保 stop() 时能等所有进行中的 btree 操作完成。

### 6.2 崩溃恢复相关的几个方法——与 `IndexWBCache::recover()` 对接

- **`repair_node(idx_buf)`**：校验 buf 是有效节点后，用 `init_node` 重新包装出 `BtreeNodePtr`，若是内部节点调用 `repair_links` 重建它的子节点链接；如果这个节点本身是新 root（它的 `m_up_buffer` 是 meta buf），再调用 `on_root_changed` 把新 root 写回 superblock。
- **`repair_root_node(idx_buf)`**：处理"root collapse" 场景——检测到 `idx_buf` 就是 SB 记录的旧 root，且它还留着一个 `next_bnode`（在 collapse 前，旧 root 临时指向新晋升的 edge 节点），于是把这个 `next_bnode` 转正为 `edge_value`，重新持久化。这里还对接了 `fault_cmt_service`（故障遏制服务）：如果发现 root 本不应该有 edge 但却有，会触发 `FaultContainmentEvent::ENTER` 而不是直接 crash，这是比纯 assert 更"优雅降级"的容错路径。
- **`set_root_from_committed_buf`**：处理 root split 场景，把某个已确认持久化的候选节点提升为新的 SB root，并立即 `m_sb.write()` 落盘——因为"即使本次强制 CP 里没有任何脏 buffer，这个恢复期间的 root 提升动作也必须自己扛住下一次崩溃"。
- **`delete_stale_children`/`delete_stale_links`**：遍历内部节点所有子条目，发现 `child_node->is_node_deleted()` 的直接从父节点里摘除该条目（并相应调整 edge/next_bnode），这是处理"父节点已经写盘但其某个子节点在崩溃前已被标记删除"的场景。
- **`repair_links`（整个文件里最长、最复杂的函数）**：当一个内部父节点在恢复后需要重建其子节点索引时调用。算法思路：
  1. 先记录修复前该父节点的所有 `{child_id -> key}` 映射（`orig_child_infos`），作为后续判断"这个子节点原本是不是属于我"的依据；
  2. 把父节点内容**全部清空**（`parent_node->remove_all`），然后**从最左侧子节点开始，沿着叶子/子节点层的 `next_bnode` 链表逐个往右走，重新插入每个还存活（未被标记删除）的子节点**，边走边判断是否已经超出本父节点的管辖范围（通过 `find_true_sibling` 找到父节点真正的右兄弟，以及 `last_parent_key` 判断越界）；
  3. 过程中如果本父节点被撑满还要继续插入，会**现场再分裂一个新的兄弟父节点**（`new_parent_nodes`），分裂完成后如果这就是 root 还要调用 `update_root` 现场长出一层新 root；
  4. 为了修复失败时能回滚，先把父节点原始字节整个 `memcpy` 备份到 `tmp_buffer`，失败时原样拷回去。

这套 `repair_links` 算法本质上是**完全不依赖"旧父节点内容是否可信"，而是重新从子节点链表的真实状态反推出父节点该长什么样**——这与 README 里提到的设计哲学一致："recovery 更偏向简单正确性而非最优性能"，哪怕要整体重建一个父节点的索引内容也在所不惜。

### 6.3 MemBtree：最简实现对照

`mem_btree.hpp` 展示了把 7 个钩子实现到最简单程度的样子：`alloc_node` 直接 `new uint8_t[]`，`node_id` 直接用对象指针转换而来（`r_cast<bnodeid_t>(new_node)`），`write_node_impl`/`refresh_node`/`on_root_changed` 全部是空操作，`transact_nodes` 只是依次同步写节点+释放被删节点——没有 CP、没有 WBCache、没有崩溃恢复，是用来做单元测试或纯内存索引的最轻量实现，也从反面印证了 `Btree<K,V>` 核心算法层与持久化/恢复机制完全解耦的设计目标。

---

## 七、小结：几个贯穿全局的设计原则

1. **原地更新 + 依赖 DAG 恢复**：节点大小等于设备原子页大小，单节点写入天然原子；多节点结构变更（split/merge）靠 `IndexWBCache` 的依赖链 + journal 保证。
2. **乐观锁 + gen 比对 + retry-from-root**：避免内部节点被悲观地长时间持有写锁，用代际号检测"锁升级窗口期是否发生了并发结构变更"。
3. **物理布局与算法解耦**：`BtreeNode` 的纯虚接口把"怎么在字节层面存储/搬运 K/V"完全留给子类，使得同一套 put/get/remove/split/merge 算法可以服务定长、变长、前缀压缩三种截然不同的物理格式。
4. **link_version 贯穿父子边**：不仅用于乐观锁校验，也是崩溃恢复判断"父节点记录的子链接是否仍然对应当前这一代子节点"的关键字段。
5. **恢复优先正确性而非效率**：`repair_links` 宁可整体清空重建父节点索引，也不做增量式的"找出哪条边错了就修哪条"的精巧优化，这是 HomeStore 团队有意识的工程取舍。

## 相关文件索引

- `src/include/homestore/btree/btree.hpp` / `btree.ipp` — `Btree<K,V>` 模板定义与公开 API 实现
- `src/include/homestore/btree/btree_kv.hpp` — Key/Value/Range 抽象
- `src/include/homestore/btree/btree_req.hpp` — Put/Get/Remove/Query 请求类型
- `src/include/homestore/btree/detail/btree_internal.hpp` — `BtreeConfig`、日志/断言宏、`btree_status_t` 等
- `src/include/homestore/btree/detail/btree_node.hpp` — `BtreeNode` 抽象基类、`persistent_hdr_t`/`transient_hdr_t`
- `src/include/homestore/btree/detail/btree_node_mgr.ipp` — 节点分配/读取/加锁/解锁/升级锁
- `src/include/homestore/btree/detail/btree_mutate_impl.ipp` — `do_put`/`split_node`/`check_split_root`
- `src/include/homestore/btree/detail/btree_remove_impl.ipp` — `do_remove`/`merge_nodes`/`check_collapse_root`
- `src/include/homestore/btree/detail/btree_query_impl.ipp` — `do_sweep_query`/`do_traversal_query`
- `src/include/homestore/btree/detail/btree_get_impl.ipp` — `do_get`
- `src/include/homestore/btree/detail/btree_common.ipp` — 遍历、销毁、校验、可视化等辅助方法
- `src/include/homestore/btree/detail/variant_node.hpp` — `VariantNode` 通用 multi_get/multi_put/multi_remove
- `src/include/homestore/btree/detail/simple_node.hpp` — `SimpleNode`（FIXED 定长布局）
- `src/include/homestore/btree/detail/varlen_node.hpp` — `VariableNode`/`VarKeySizeNode`/`VarValueSizeNode`/`VarObjSizeNode`
- `src/include/homestore/btree/detail/prefix_node.hpp` — `FixedPrefixNode`（前缀压缩布局）
- `src/include/homestore/btree/mem_btree.hpp` — `MemBtree`（纯内存实现）
- `src/include/homestore/index/index_table.hpp` — `IndexTable<K,V>`（持久化实现，含崩溃修复算法）
