# Index Service Long Running Test: Plan & Gap Analysis

---

## Part 1：Index Service Long Running 测试方案

### 1.1 总体目标

Index Service 的 long running 测试有三个层面的目标：

**正确性目标（Correctness）**

*无 crash 场景*

- 点操作（put / update / remove）：返回**成功**时，插入的 key 一定能查到，删除的 key 一定查不到，更新的 key 一定能查到最新的值；返回**失败**时，任何 key 的状态一定不变（原子性保证）。
- range 操作（range_put / range_remove）**无原子性保证**：返回**成功**时，range 内所有 key 的状态符合预期；返回**失败**时，range 内 key 处于不确定的部分修改状态，需应用层通过重试或重建修复。

*crash 场景*

- **点操作原子性不可破坏**：每个点操作要么完整生效、要么完全未生效，不允许出现 key 存在但 value 损坏、或操作部分写入的中间状态（in-flight 操作是否最终生效由耐久性语义决定，见下文）。
- **不引入幽灵数据**：恢复后，树中不会出现任何既不属于 persisted snapshot、也未通过任何 in-flight 操作写入的 key。
- **range 操作 crash 后语义与失败一致**：crash 后 range 内 key 的状态不确定（与无 crash 时 range 操作失败的语义相同），上层需通过重试或 range_query 重建来恢复确定状态。

**耐久性目标（Durability Semantics Validation）**

HomeStore index 的持久化语义：**操作返回成功 ≠ 数据已落盘**，成功仅表示操作已记录在当前 CP 的内存 checkpoint 中。测试必须验证以下三点：

- **in-flight 操作不保证存在**：上次 CP flush 完成后发起的操作（in-flight），crash 后其结果不保证存在；系统仅保证恢复至上次成功 CP flush 的状态，不保证更新。
- **已持久化数据不丢失**：经历任意时机的 crash（包括 CP flush 的各个中间阶段）后，系统能正确恢复，恢复后的状态与上次成功 CP flush 的"已持久化"状态完全一致，不允许已落盘数据丢失。
- **测试必须区分两类状态**：shadow map 须同时维护 persisted snapshot（上次 CP 成功后的状态）和 in-flight 操作集合，并对二者分别作出正确断言，不能混为一谈。

**稳定性目标（Stability）**
- 长时间（数小时至数天）运行无内存泄漏、没有明显性能退化。

---

### 1.2 核心原则

#### 原则一：持久化语义是测试的基础

HomeStore index 的操作语义规定：

> **点操作（put / remove / update）**：
> - 返回**成功**：修改已记录在内存 checkpoint 中，但**不保证**已通过 CP flush 写入磁盘。
> - 返回**失败**：操作未被应用，内存状态完全不变（原子性保证）。
>
> **range 写操作（range_put / range_remove）**：
> - 返回**成功**：整个 range 内所有 key 的修改已记录在内存 checkpoint 中。
> - 返回**失败**：已处理的 key 集合未知，range 处于不确定的部分修改状态——**无原子性保证**。
>
> **remove_any**（`BtreeRemoveAnyRequest`）：
> - 从指定 range 内删除任意一个 key；操作成功后通过 `out_key` 返回实际删除的 key。
> - 返回**成功**：恰好有一个 key 被删除，shadow map 必须在读取 `out_key` 后才能确定哪个 key 被删除，然后将其从 shadow map 中移除。
> - 返回**失败**：range 内无 key 可删，或 range 为空，树状态不变。
> - **与 range_remove 的本质区别**：range_remove 删除 range 内所有 key（批量），remove_any 只删除一个（由 B-tree 自主选择）；shadow map 更新逻辑完全不同。
>
> **get_any**（`BtreeGetAnyRequest`）：
> - 从指定 range 内获取任意一个 key/value；只读操作，不修改树。
> - 返回**成功**：返回 range 内某个 key/value，结果与 shadow map 对应 range 中任意一个存在的 key 一致即为正确（不要求返回特定 key）。
> - 返回**失败**：range 内无 key，树状态不变。
>
> **range_query**：
> - 返回**成功**：返回 range 内所有符合条件的 key/value，树状态不受影响。
> - 返回**失败**：查询未完成，但树状态不受影响（range_query 不修改树）。

这意味着：
- 若在操作成功但 CP flush 完成之前发生 crash，重启后这些操作的结果**可能不存在**。
- 对于 range 操作，即使 range 操作已成功完成才发生 crash，成功写入的 key 仍然遵循同样的"成功 ≠ 落盘"语义。
- 测试的 shadow map（ground truth）必须分为两个层次：
  - **已持久化快照（persisted snapshot）**：上一次 CP flush 成功完成后保存到文件的状态，这是 crash 之后的"最低保证"。
  - **内存中的未持久化操作（in-flight operations）**：当前 CP 周期内已成功返回但尚未落盘的操作，crash 后这些操作的结果不保证存在，需要 reapply。

断言策略（点操作）：
- **crash 前**：验证树中的 key 集合与 shadow map 完全一致（shadow map 中所有 key 在树中存在，且树中不含 shadow map 之外的幽灵 key——双向验证）。
- **crash 后、reapply 前**：树的 key 集合处于 persisted snapshot 与"persisted snapshot + 本轮全部 in-flight 操作"之间的某个中间态。具体来说：
  - CP flush 已写入磁盘的 buffer 所涉及的操作（put/remove）在恢复后**可能已生效**，因此树中可以出现 persisted snapshot **里没有的 key**（in-flight put 已部分落盘），也可能缺少 persisted snapshot 中有的 key（in-flight remove 已部分落盘）。
  - 唯一的强保证：persisted snapshot 中存在且在本轮操作中**未被 remove** 的 key，在恢复后必须仍然存在（不能有已持久化的数据丢失）。
  - 实践中对 in-flight 操作涉及的 key 不做精确断言，仅对"稳定集合"（persisted snapshot 中未被本轮 **remove** 操作涉及的 key）执行 sanity check。UPDATE 操作涉及的 key 已在 persisted snapshot 中落盘，crash 只影响其 value，不影响 key 存在性，仍在检查范围内；INSERT 操作引入的新 key 不在 persisted snapshot 中，自然不被迭代到，无需显式排除。
- **reapply 后**：树与最新的 shadow map 完全一致。

**range 操作的特殊处理**：

range_put / range_remove 在失败时无原子性保证；range_query 是只读操作不修改树状态。两者语义均与点操作有本质区别，需要单独设计。

- **shadow map 更新**：range_put / range_remove 返回成功时，shadow map 将整个 range 的变更一次性更新；返回失败时，shadow map 不更新（已处理集合未知，无法推断）。

- **range_query 验证**：range_query 单次调用返回整个 range 内的所有结果，直接与 shadow map 中对应 range 的 key 集合比较。

- **crash 后 reapply**：若 crash 发生在 range_put / range_remove 执行中途或完成后 CP flush 之前，reapply 时需对整个 range 从头重新执行：对于 range_put，使用 force_upsert 重复写入（幂等，不影响已有数据）；对于 range_remove，重新执行 remove，若 key 已在 crash 时随 buffer 落盘而被删除，remove 返回 not_found 属正常情况，应忽略。

- **失败处理**：range 操作返回失败时，range 内 key 的状态不确定（已处理集合未知），shadow map 无法根据操作入参推断更新内容。测试作为"应用层"应采取以下策略之一，使 shadow map 回到确定状态后再继续后续操作：（1）**重试**：重新发起整个 range 操作直至成功，shadow map 仅在最终成功时更新；（2）**重建**：对该 range 执行一次 range_query，以实际查询结果重建 shadow map 中该 range 的状态（若 range_query 本身返回失败，视为测试框架发现 bug，立即终止并报错，不继续后续操作）。

#### 原则二：Fault Injection 必须覆盖两类失败

Fault Injection 包含两种本质不同的失败类型，必须分别设计：

| | 进程 Crash | IO 错误 |
|---|---|---|
| 进程状态 | 已死，需重启 | 仍在运行 |
| 系统响应 | crash recovery + reapply | 错误传播 + 继续或降级运行 |
| 测试目标 | 恢复后数据的正确性 | 错误是否被正确传播、不被静默吞掉 |
| 实现方式 | btree 层 flip（现有机制） | vdev/physical_dev 层 flip（待实现） |

**类型一：进程 Crash（现有 flip 机制）**

B-tree 的结构操作在 CP flush 时会写入多个 buffer，crash 可能发生在任意一个 buffer 写入的瞬间。必须覆盖以下注入点：

*Split 路径*（插入 key 导致节点分裂）：
- `crash_flush_on_split_at_parent`：在写父节点 buffer 时 crash
- `crash_flush_on_split_at_left_child`：在写左孩子 buffer 时 crash
- `crash_flush_on_split_at_right_child`：在写右孩子 buffer 时 crash
- `crash_flush_on_root`：在 root 分裂（MetaIndexBuffer 为父节点）时 crash

*Merge 路径*（删除 key 导致节点合并）：
- `crash_flush_on_merge_at_parent`：在写合并后父节点 buffer 时 crash
- `crash_flush_on_merge_at_left_child`：在写合并后左孩子 buffer 时 crash
- `crash_flush_on_freed_child`：在写被释放的右孩子 buffer 时 crash

**类型二：IO 错误注入（待实现）**

IO 错误发生时进程仍在运行，错误处理路径与 crash recovery 完全不同，必须单独验证。

*写错误（Write IO Error）*：CP flush 阶段写 node buffer 时返回 EIO。
- 期望行为：CP 失败，向触发方返回错误；进程不崩溃；系统状态回退到上次成功 CP，后续 CP 仍可正常触发和完成。
- 验证：写错误后调用 get_all()，结果应与上次 persisted snapshot 一致。

*读错误（Read IO Error）*：执行 put/remove/get/query 读取某个 B-tree node 时返回 EIO。
- 期望行为：操作返回明确的错误码（不是 success，不返回错误数据）；错误不被静默吞掉；其他不涉及该节点的操作不受影响。
- 验证：对受影响 key 的操作返回 IO 错误；shadow map 中其他 key 仍可正常读写。

实现上需要在 vdev 层增加 flip 支持（`inject_write_io_error` / `inject_read_io_error`），注入层次比现有 crash flip 更低。

#### 原则三：随机性与确定性并重

随机测试和确定性测试各有不可替代的价值，必须同时使用，不能偏废其一。

**为什么需要随机性**

B-tree 的 split 和 merge 行为取决于 key 的分布、插入/删除的顺序、节点的当前填充程度。这些组合形成的状态空间极大，任何人工设计的测试序列都只能覆盖其中很小一部分。随机测试能以较低成本探索这个空间，发现人工设计测试未曾预料到的场景。例如：

- 随机 key 分布可能恰好使某个 internal node 在 split 的同时另一个 internal node 发生 merge，形成级联结构变更，而这种组合在确定性测试中极难构造。
- 随机 put/remove 比例的变化使树的大小在不同阶段处于不同填充率，暴露只在特定填充率下才会触发的 corner case。

**随机性的具体维度**

随机化的对象不仅是"选哪个 key"，还包括多个维度：

1. **key 选择**：使用均匀分布或有偏分布（如集中在已有 key 附近）来模拟不同访问模式。
2. **操作比例**：put/remove 的比例影响树的整体大小趋势，50%/50% 使树在平衡状态下来回振荡，70%/30% 使树持续增长并触发更多 split。
3. **flip 注入概率**：当前代码用 90% 概率注入 crash、10% 正常执行，这使得测试路径中包含正常执行场景：（1）覆盖正常 CP 代码路径，防止遗漏 CP 成功时的行为；（2）降低整体测试耗时，crash + 重启的代价远高于正常 CP 完成。
4. **flip 的轮换顺序**：在循环遍历 flip 列表的基础上，可以随机打乱顺序，使每种连续两轮的 flip 组合都有机会出现（既能出现 A→B，也能出现 B→A），避免固定循环导致某些组合永不触发。

**为什么随机测试单独不够——可复现性问题**

随机测试发现 bug 后，如果无法复现，几乎等于没有发现。必须在随机性的同时保证完全可复现：

- **种子（seed）管理**：每次测试启动时记录随机种子到日志（当前代码已实现：`LOGINFO("Using seed {}", seed)`）。复现时用 `--seed` 参数指定相同种子即可重走完全相同的路径。
- **操作序列持久化**：`SequenceGenerator::save_to_file` 将每 Round 的操作序列保存到 `/tmp/operations_N.txt`，`/tmp/flip_history.txt` 记录每轮使用的 flip 名称。失败后使用 `--load_from_file` 模式，不依赖种子重放，可以在不同机器上精确复现（种子依赖系统随机数实现，不同平台可能不同）。
- **两级复现策略**：种子 → 粗粒度复现（适合在同一机器快速复现）；操作序列文件 → 细粒度复现（适合跨机器、跨时间的精确复现）。

**为什么需要确定性测试**

某些高风险场景的概率极低，随机测试在合理时间内无法保证触发：

- **左边缘插入触发 split**（`SplitOnLeftEdge`）：只有当新插入的 key 比树中所有 key 都小时才会触发，对于均匀随机 key 分布，触发概率约为 1/N，树有 100 万条目时概率仅有百万分之一。
- **Root split**：只发生在树只有一个节点且该节点恰好满时，随机插入场景中只在最初几步有机会触发，之后树已经有多层结构，不会再发生 root split。
- **多层级联 merge**：需要特定的 key 分布使多个 sibling 节点同时低于 min_keys 阈值，随机删除几乎不会同时触发 level 0 和 level 1 的 merge。

确定性测试通过精心构造操作序列，**保证**这些场景被覆盖，不依赖概率。

**确定性测试的设计原则**

确定性测试应当是最小化的：使用尽量少的 key（如 20~100 个），配合小的 max_keys（如 5），使树在少量操作内就能触发目标结构变更。这样做的好处是：
1. 测试快，便于调试（失败时能直接看到 `visualize_keys` 生成的完整树结构）。
2. 逻辑清晰，能精确定位到某个 flip 点的行为。
3. 容易转化为回归测试——发现 bug 后，用发现 bug 的那个随机序列文件固化为确定性测试，永久保留在回归集中。

**随机测试与确定性测试的分工**

```
确定性测试（单元/集成测试）
  └─► 保证已知高风险场景被覆盖
  └─► bug 修复后立即固化为回归测试

随机 long running 测试
  └─► 探索未知的场景组合
  └─► 暴露多个结构操作交织时的竞态和恢复错误
  └─► 发现 bug → 保存操作序列 → 转化为确定性回归测试
```

两者形成闭环：随机测试发现的 bug 经固化后补充到确定性测试集，使确定性测试集随时间持续增长，覆盖范围不断扩大。

#### 原则四：测试的组合性

Long running 测试不是简单的"跑一个 case 很长时间"，而是多个测试场景的**有序组合**：
1. 纯并发操作（无 crash）验证并发正确性基线
2. Clean shutdown / restart 验证正常持久化路径
3. Crash at split 验证插入路径容错
4. Crash at merge 验证删除路径容错
5. Mixed put+remove + crash 验证复杂场景
6. 多 Index Table 并发 crash 验证共享 CP context 的恢复正确性
7. 长时间稳定性监控验证无内存泄漏、无性能退化
8. IO 错误注入验证错误处理路径的正确性
9. CRC 不匹配数据完整性测试验证静默损坏的检测能力

---

### 1.3 主要测试方案

#### 方案 A：并发全操作长时测试（Concurrent All-Ops）

**目的**：验证多线程、多 fiber 并发执行各类操作时的正确性。

**测试对象**：`test_index_btree` — `BtreeConcurrentTest.ConcurrentAllOps`

**参数设计**：
- 线程数：2~8，fiber 数：2~8（覆盖不同并发度）
- 条目数：200 万（2M），预加载：256K
- 运行时长：4~8 小时
- 操作比例（默认，相对权重而非绝对百分比）：put 18、remove 14、range_put 20、range_remove 2、query 10
- 开启周期性 CP flush（`cache_max_throttle_cnt` 限制触发自动 CP）

**验证点**：
- 点操作**成功**时，shadow map 同步更新；树中 key 状态与 shadow map 完全一致（成功 put → key 可查，成功 remove → key 不存在，成功 update → key 为最新值）
- 点操作**失败**时，shadow map 不更新；树中 key 状态应与 shadow map 保持一致（失败操作不改变任何 key 的状态，原子性保证）
- range_put / range_remove 返回成功后，shadow map 一次性更新整个 range 的变更，并验证 range 内所有 key 的状态与 shadow map 一致
- range_query 单次调用返回整个 range 的结果，直接与 shadow map 比对
- range 操作返回失败时，通过重试（重新发起直至成功）或重建（对该 range 执行一次 range_query）使 shadow map 与树重新同步
- 无 crash、无 assert、无内存异常

**重启场景**：在 4 小时测试中，每 30 分钟做一次 clean shutdown + restart，验证状态能从磁盘正确恢复：
- 重启后 shadow map（从文件加载）与树中 key 数量一致
- 执行全量 get_all() 验证每个 key 的 value 正确

---

#### 方案 B：Clean Shutdown 循环测试

**目的**：验证正常关机场景下持久化的正确性，数据在多次重启之间保持一致。

**设计**：循环共 N 轮（N=10），每轮运行 T/N 时间，总时长 T：
1. 第 1 轮：init_device=true，运行 T/N 时间，clean shutdown
2. 第 2～N 轮：init_device=false（复用已有设备），运行 T/N 时间，clean shutdown；每轮重启后验证：
   - shadow_map.size() == btree.count_keys(root)（key 数量一致）
   - 对所有 key 执行 get_all() 全量验证

---

#### 方案 C：Crash Recovery — 纯插入（Put Only）

**目的**：在插入密集场景下，测试 CP flush 各阶段 crash 后的恢复正确性。

**Crash Flip 轮换**：

Flip 按固定顺序循环，确保每个注入点都被均等覆盖：
```
Round 1:  crash_flush_on_split_at_parent
Round 2:  crash_flush_on_split_at_left_child
Round 3:  crash_flush_on_split_at_right_child
Round 4:  crash_flush_on_root
（循环）
```

并非每轮都注入 crash：以 90% 概率注入 flip、10% 概率正常执行：（1）覆盖正常 CP 代码路径；（2）降低整体测试耗时（crash + 重启代价远高于正常 CP）。

**每 Round 流程**：
1. 生成 N 个 put 操作（随机 key，种子记录到日志）
2. 将本轮操作序列保存到文件（`save_to_file`，**始终开启**）
3. 执行操作，更新内存 shadow map
4. 按 90% 概率决定是否注入 flip；若注入，将本轮 flip 名称追加到 `/tmp/flip_history.txt`
5. 触发 CP，若注入了 flip 则等待 crash + homestore 重启，否则等待 CP 完成
6. 验证稳定集合（persisted snapshot 中未被本轮 remove 操作涉及的 key）全部存在
   - crash 路径（90%）：树处于中间态，此断言有实际意义
   - 正常 CP 路径（10%）：CP 已完成，断言自然成立，无效果
7. 对本轮 in-flight put 操作执行 reapply（force_upsert，幂等）
   - crash 路径：必要，将未完全持久化的 put 补齐
   - 正常 CP 路径：步骤 5 的 CP 已将所有 in-flight 操作持久化，reapply 对树状态无影响（force_upsert 以相同值覆盖已存在 key，幂等无副作用）
8. 触发 CP flush，保存新的 persisted snapshot
9. 全量 get_all() 验证

**参数**：1.28M 条目，preload=1024，每 Round 100 个操作，1000 Round，运行 2 小时。

---

#### 方案 D：Crash Recovery — 纯删除（Remove Only）

**目的**：在删除密集场景（触发大量 merge）下测试恢复正确性。

**前提**：先将树填满（preload = num_entries），然后进行纯删除。

**参数**：100K 条目，preload = num_entries（满树），min_keys=2，max_keys=10，每 Round 100 个 remove，1000 Round，运行 2 小时。

**Crash Flip**：
- `crash_flush_on_merge_at_parent`
- `crash_flush_on_merge_at_left_child`
- `crash_flush_on_freed_child`（当前测试中已注释，需启用）

**注入概率**：同方案 C，以 90% 概率注入 flip、10% 概率正常执行：（1）覆盖正常 CP 代码路径；（2）降低整体测试耗时（crash + 重启代价远高于正常 CP）。

**特殊考量**：
- 删除后节点合并可能触发多层 merge（level 0 → level 1 → ...），需要用小节点容量（max_keys=10, min_keys=2）来频繁触发
- 测试三种删除顺序，覆盖确定性和随机性两类场景（对应原则三）：
  - **正向顺序**（确定性）：从小到大删除，使最左侧 sibling 率先触发 merge
  - **反向顺序**（确定性）：从大到小删除，专门触发右侧 merge 和 `freed_child` 路径
  - **随机顺序**（随机性）：随机 key 选择，探索前两种顺序无法覆盖的中间节点 merge 组合
- 多层级联 merge（level 0 → level 1）是概率极低的场景，应用确定性测试精确构造（小树 + 特定 key 删除序列），参考 `MergeRemoveBasic` 的设计
- **树被删空的处理**：当树中所有 key 均被删除后，纯删除测试无法继续。此时应重新 preload（将树重新填满至 num_entries），同步清空并以新 preload 的内容重建 shadow map，随后触发 CP flush 并重新保存 persisted snapshot 文件（以新满树状态作为后续 crash 断言的基准），然后继续下一轮删除 + crash 测试，直到总运行时长到达上限。

---

#### 方案 E：Crash Recovery — 混合读写（Put + Remove + Crash）

**目的**：混合操作下，同时触发 split 和 merge，随机 crash 并恢复。

**Crash Flip 策略**：
- 以 90% 概率注入 crash、10% 概率正常执行，理由同方案 C：（1）覆盖正常 CP 代码路径；（2）降低整体测试耗时（crash + 重启代价远高于正常 CP）。
- Flip 选取：split flip 列表和 merge flip 列表各自独立顺序循环（`cur_put_flip_idx++ % put_flips.size()`），两组 flip 同时 armed——CP flush 时哪个结构操作先被触发，对应 flip 先触发 crash。此为**顺序循环而非随机选取**，意味着 split flip A 总在 split flip B 之前出现，不同 flip 的组合顺序是固定的。这是当前实现的一个局限（参见差距 G9）。
- 所有操作序列和 flip 选择均应保存到文件（`save_mode` 开启），以保证失败场景可跨机器复现。

**参数**：100K 条目，preload=1024，min_keys=3，max_keys=10，2 小时，1000 Round，每 Round 100 个操作（50 put + 50 remove）。

**put_freq = 50%**：保持大约一半插入、一半删除的节点总量平衡，使树的大小长期维持在中等填充率，同时持续触发 split 和 merge。

---

#### 方案 F：多 Index Table 并发 Crash 测试

**目的**：验证多个 index table 共享同一 CP 上下文时，crash 恢复的正确性。背景是 `txn_journal` 中可能同时包含多个 table 的记录，若 `MetaIndexBuffer` 的 blkid 碰撞（SDSTOR-21880），会导致恢复时 ordinal 匹配失败。

**参数**：2~4 个 index table，每表 50K 条目，preload=512，min_keys=3，max_keys=10，运行 2 小时，每 Round 每表 50 个操作（put 与 remove 各约 50%，以同时触发 split 和 merge，确保各 table 的 dirty buffer 多样化交织在同一 CP epoch 内）。

**Crash Flip**：使用全部 split flip（含 `crash_flush_on_root`）和 merge flip，各 table 共享同一 flip 配置——重点是在同一 CP epoch 内多个 table 的 buffer 交织写入时触发 crash，而非各 table 独立 crash。

**设计**：
1. 创建 2~4 个 index table，每表维护独立的 shadow map 和 persisted snapshot 文件
2. 并发向所有 table 执行混合读写操作（put 触发 split/root split，remove 触发 merge），确保同一 CP epoch 内各 table 均有来自不同结构操作类型的 dirty buffer
3. 在 CP flush 期间注入 crash flip，使 crash 发生在多个 table 的 buffer 混合写入的中间阶段
4. 重启后对每个 table 独立执行：sanity_check 稳定集合、reapply diff、全量 get_all()
5. 验证各 table 恢复后状态互不影响（一个 table 的恢复错误不污染另一个 table 的数据）

---

#### 方案 G：长时间稳定性测试（Stability / Memory Leak Detection）

**目的**：24 小时甚至更长时间运行，监控资源使用趋势。

**监控指标**：
- 进程内存使用（RSS）：在 WBCache 达到稳定容量后，RSS 不应持续单调增长（允许因 cache 预热导致的初期增长，关注的是 1 小时后是否仍在上涨）
- B-tree 节点数（interior + leaf）：应与当前 key 数量成比例；在 put/remove 比例稳定时，节点数不应无限制增长（潜在节点泄漏信号）
- 树深度：不应在 key 数量不变的情况下持续增大（树深度异常增大意味着 split/merge 不平衡）
- CP 耗时：每次 CP flush 的耗时（ms）不应随运行时间单调增长；若 24 小时后 CP 耗时超过初始值的 2 倍，视为性能退化

**实现**：每 5 分钟记录一次上述指标，追加到日志文件，测试结束后用脚本检查趋势（简单方式：对比前 1 小时均值与后 1 小时均值，增长超过 20% 触发告警）。

---

#### 方案 H：IO 错误注入测试

**目的**：验证 B-tree 操作和 CP flush 路径在 IO 错误场景下的错误处理正确性，覆盖 crash 测试无法触达的错误传播路径。

**子场景 H1：CP flush 写错误**

1. 预加载若干 key 并触发 CP，保存 persisted snapshot
2. 执行若干 put/remove 操作
3. 在 CP flush 的 node buffer 写入路径上注入 write EIO flip
4. 触发 CP，预期 CP 失败
5. 验证：进程未崩溃；调用 get_all() 结果与 persisted snapshot 一致（写错误导致 CP 回退，不引入新数据）
6. 移除 flip，再次触发 CP，预期成功
7. 验证：所有操作的结果现在均可读到

**子场景 H2：B-tree 节点读错误**

1. 预加载若干 key，充分触发 split/merge 使树有多层节点
2. 对某个 internal/leaf 节点注入 read EIO flip
3. 对该节点范围内的 key 执行 get/put/remove
4. 验证：操作返回 IO 错误而非 success；不返回脏数据；ASSERT/ABORT 不触发
5. 验证：对其他不涉及该节点的 key 操作正常（隔离性）

**实现前提**：需要在 vdev/physical_dev 层新增 flip 注入点（类似现有 `crash_flush_on_*` flip 的机制，但不触发进程崩溃，而是让 IO 调用返回错误码）。

---

#### 方案 I：CRC 不匹配数据完整性测试（Data Integrity）

**目的**：验证 B-tree 节点数据在磁盘上发生静默损坏（如位翻转）时，系统能正确检测并向调用方返回 `crc_mismatch`，而非使用损坏数据继续运行或产生崩溃。此测试是方案 H 的补充——方案 H 测试 vdev 层 IO 调用失败；本方案测试 IO 调用成功但读回数据已损坏的情形。

**与方案 H 的区别**：

| | 方案 H（IO 错误） | 方案 I（CRC 不匹配） |
|---|---|---|
| 触发层次 | vdev 层 IO 调用失败 | IO 成功但数据损坏 |
| 返回码 | `node_read_failed` | `crc_mismatch` |
| 模拟场景 | 磁盘/控制器 IO 错误 | 磁盘位翻转、写入错误 |

**测试流程**：

1. 预加载足够多的 key，触发 split 使树拥有多层节点（interior + leaf）
2. 触发 CP flush，确保所有节点已落盘
3. 注入损坏——通过以下任一方式模拟节点数据损坏：
   - **读取时损坏 flip**：在 B-tree 读取路径上增加 flip，节点从磁盘读入内存后、CRC 校验前翻转若干字节
   - **直接磁盘损坏**：通过原始文件 I/O 对已落盘节点翻转若干 payload 字节（保持 offset 正确使节点可被定位，但 CRC 不再匹配）
4. 对损坏节点范围内的 key 执行 get / put / remove
5. 验证：
   - 操作返回 `crc_mismatch`，不返回 `success`，不返回错误数据
   - 不触发 ASSERT / ABORT（错误被优雅处理而非 crash）
   - 损坏节点以外的 key 操作正常返回 `success`（隔离性）
6. 分别覆盖 leaf 节点损坏和 interior 节点损坏两种情形（interior 损坏影响范围更广）

**实现前提**：需要在 B-tree 读取路径增加 flip 注入点（在 CRC 校验前注入数据损坏），或测试框架直接操作底层文件。

---

### 1.4 Shadow Map 与验证机制的详细设计

Shadow map 是所有 crash recovery 测试的核心。其工作机制如下：

```
┌──────────────────────────────────────────────────────────────────────┐
│                        一次 Round 的状态流                            │
│                                                                        │
│  persisted_snapshot (文件，上次 CP 成功后保存)                          │
│       │                                                                │
│       ▼                                                                │
│  in-memory shadow_map = persisted_snapshot + 本 Round 操作             │
│       │                                                                │
│       ├─► 执行 put/remove 操作 → 更新 shadow_map                       │
│       │                                                                │
│       ├─► 触发 CP (crash flip 激活) → homestore crash                  │
│       │                                                                │
│       └─► 重启后（crash recovery 完成）：                               │
│                                                                        │
│             树的状态 = persisted_snapshot ± 部分 in-flight 操作         │
│             （in-flight put 的 buffer 若已落盘则 key 可能出现在树中；    │
│              in-flight remove 的 buffer 若已落盘则 key 可能已被删除）    │
│                                                                        │
│             断言（sanity_check）：                                      │
│               persisted_snapshot 中未被本轮 in-flight remove 操作     │
│               涉及的 key 必须全部存在于树中（强保证，不可缺失）           │
│                                                                        │
│             diff = shadow_map - persisted_snapshot                    │
│             对 diff 中的每个操作执行 reapply：                          │
│               put/update → force_upsert（幂等）                        │
│               remove → re-remove（not_found 视为正常）                 │
│             触发 CP → 保存新 persisted_snapshot                         │
│             get_all() → 树中内容 == shadow_map   (最终断言)              │
└──────────────────────────────────────────────────────────────────────┘
```

**重要细节**：

- `persisted_snapshot` 必须在每次 CP flush **成功完成后**立即保存到文件（`shadow_map.save(filename)`），而不是在操作完成后保存。否则 crash 后加载的文件可能包含未持久化的操作。
- `reapply_after_crash` 对于 diff 中的 **put/update 操作**需要使用 `force_upsert`（幂等 put）而不是普通 insert，因为该 key 在 crash 前可能已经部分落盘；对于 diff 中的 **remove 操作**，需要重新执行 remove——若该 key 已在 crash 时随部分 buffer 落盘（已被删除），remove 返回 `not_found` 属于正常情况，应当忽略而非视为错误。
- 对 persisted snapshot 中的 key 执行 sanity check 时，需排除本轮 in-flight **remove** 操作涉及的 key（这些 key 是否已随 buffer 落盘而被删除不确定）。in-flight **update** 操作涉及的 key（这些 key 在 persisted snapshot 中已存在）仍须断言存在，因为 crash 只影响其 value 是否更新，不影响 key 的存在性。in-flight **insert** 操作引入的新 key 本不在 persisted snapshot 中，自然不在 sanity check 的迭代范围内，无需显式排除，也不应断言其存在（crash 前未经 CP 落盘，crash 后不保证可见）。

---

### 1.5 实现建议

1. **测试执行框架**：Python 编排脚本（index_test.py）负责参数化和串联不同测试场景；C++ 测试二进制负责具体的 B-tree 操作和验证。两层分工清晰。

2. **Flip 管理**：每个 crash round 只注入一个或一组相关 flip，确保 crash 的根因是已知的，便于调试。`set_basic_flip(name, count=1, percentage=100)` 确保 flip 触发一次后自动清除。每轮以 90% 概率注入 flip，剩余 10% 正常执行：（1）覆盖非 crash 的正常 CP 代码路径；（2）降低整体测试耗时（crash + 重启远慢于正常 CP 完成）。Flip 选取当前为顺序循环，若需要覆盖 flip 的组合顺序效应，应改为随机选取（参见差距 G9）。

3. **可复现性（两级策略）**：
   - **第一级（种子）**：测试启动时记录随机种子到日志（`LOGINFO("Using seed {}", seed)`），同机器复现时用 `--seed` 参数重走相同路径。适合快速本地复现。
   - **第二级（操作序列文件）**：`save_mode` 在所有 long running 测试中**必须始终开启**。每 Round 的操作序列保存到 `/tmp/operations_N.txt`，flip 选择记录到 `/tmp/flip_history.txt`。失败后使用 `--load_from_file` 模式，不依赖种子，可在不同机器、不同时间精确复现。第二级是跨机器和跨时间复现的唯一可靠手段。
   - 两级互补：种子复现速度快但平台相关；文件复现慢但完全确定。

4. **树可视化**：关键断言失败时自动生成 `.dot` 文件（`visualize_keys`），便于人工分析树结构。

5. **参数化**：通过 Python 脚本控制 `max_keys_in_node` 和 `min_keys_in_node`，用小节点容量（如 max=10, min=2）来更频繁地触发 split/merge，加速暴露 corner case。

6. **Bug 固化为回归测试**：随机 long running 测试发现 bug 后，按以下流程将其转化为确定性回归测试，防止 bug 再次引入：
   - 保存发现 bug 的完整操作序列文件（`/tmp/operations_*.txt` + `/tmp/flip_history.txt`）
   - 在 `test_index_crash_recovery.cpp` 中新增一个 `TYPED_TEST`，用 `load_from_file` 模式加载该序列
   - 确认该 test 在修复前失败、修复后通过，作为永久回归 case 保留
   - 此流程使确定性测试集随时间持续增长，覆盖范围不断扩大

---

## Part 2：index_test.py 现有测试分析及差距

### 2.1 现有测试详情

#### `long_runnig_index`

调用 `test_index_btree: BtreeConcurrentTest/FixedLenBtree.ConcurrentAllOps`

- **测试内容**：多线程、多 fiber 并发执行混合操作（put/remove/range_put/range_remove/query），运行 4 小时，2M 条目，256K 预加载，2 threads，2 fibers
- **验证机制**：操作过程中 shadow map 跟踪，多线程操作结束后全量验证
- **不包含**：crash、clean restart

---

#### `long_running_clean_shutdown`

- **测试内容**：将 run_time 分成 10 份，第 0 轮 init 设备，后续 9 轮 `init_device=False` 复用设备。每轮结束执行 clean shutdown，下一轮启动时自动恢复
- **验证机制**：每轮恢复后 shadow map 与树的 key 数量相同，并执行全量 get_all()
- **不包含**：crash 恢复场景

---

#### `long_running_crash_put`

调用 `IndexCrashTest/FixedLenBtree.long_running_put_crash`

- **参数**：1.28M 条目，preload=1024，2 小时，1000 Round，每 Round 100 个操作
- **测试内容**：纯 put（100% insert），轮流注入 split crash flips：
  - `crash_flush_on_split_at_parent`
  - `crash_flush_on_split_at_left_child`
  - `crash_flush_on_split_at_right_child`
- **验证机制**：每次 crash 后，验证 persisted snapshot 中未被本轮 in-flight **remove** 操作涉及的 key 全部存在（sanity_check），reapply diff，全量 get_all()
- **注意**：未测试 `crash_flush_on_root`（root split 场景）

---

#### `long_running_crash_remove`

调用 `IndexCrashTest/FixedLenBtree.long_running_remove_crash`

- **参数**：100K 条目，preload = num_entries（满树），2 小时，每 Round 100 个 remove
- **测试内容**：纯 remove（100% delete），注入 merge crash flips：
  - `crash_flush_on_merge_at_parent`
  - `crash_flush_on_merge_at_left_child`
- **不包含**：`crash_flush_on_freed_child`（代码中已注释掉）

---

#### `long_running_crash_put_remove`

调用 `IndexCrashTest/FixedLenBtree.long_running_put_remove_crash`

- **参数**：100K 条目，preload=1024，min_keys=3，max_keys=10，2 小时，1000 Round，每 Round 100 个操作（50 put + 50 remove）
- **测试内容**：50% put + 50% remove，同时轮换 split 和 merge crash flips（各自独立 cycle）
- **90% 概率注入**：确保测试路径中也有正常执行（不 crash）的场景
- **验证机制**：每次 crash 后执行与 `long_running_crash_put` 相同的三步验证：sanity_check 稳定集合（persisted snapshot 中未被本轮 **remove** 操作涉及的 key 全部存在）、reapply diff（put 用 `force_upsert`，remove 用幂等 remove）、全量 get_all()

---

#### `long_running` 全流程

当前的 `long_running()` 函数执行顺序：
1. `long_runnig_index(type=0)` — 4 小时并发测试
2. `long_running_clean_shutdown(type=0)` — 10 次 clean restart
3. `long_running_crash_put_remove` × 5 轮
4. `long_running_crash_remove` × 5 轮
5. `long_running_crash_put` × 5 轮
6. `long_runnig_index()` — 再跑一次并发测试
7. `long_running_clean_shutdown()` — 再跑一次 clean restart

---

### 2.2 与理想方案的差距分析

#### 差距 1：`crash_flush_on_root` 未在 long running 中覆盖

**现状**：`long_running_put_crash` 的 flip 列表中没有 `crash_flush_on_root`，虽然 `CrashBeforeFirstCp` 和 `SplitOnLeftEdge` 等单元测试有覆盖，但长时间场景下的 root split 崩溃未被测试。

**影响**：Root split 是将单节点树分裂为多层结构的特殊操作，其 CP flush 路径与普通 split 不同（`MetaIndexBuffer` 为父节点）。多表并发时碰撞风险最高（SDSTOR-21880）。

**建议**：在 `long_running_crash_put` 的 flip 列表中加入 `crash_flush_on_root`。

---

#### 差距 2：`crash_flush_on_freed_child` 被注释掉

**现状**：`long_running_remove_crash` 的代码中 `crash_flush_on_freed_child` 被注释（`/* "crash_flush_on_freed_child" */`），实际运行中该 flip 不会被触发。

**影响**：freed child 的 flush crash 是 merge 路径的第三个关键节点，不覆盖会遗漏此处的恢复 bug。

**建议**：调查注释原因（是否有已知 bug 未修复），修复后启用该 flip。

---

#### 差距 3：Long running 测试只覆盖 FixedLenBtree，其余 4 种类型均未覆盖

代码中共有 5 种 BTree 类型，各自有不同的 key/value 长度特性和节点存储布局：

| 类型 | Key | Value | Leaf 节点 | Interior 节点 |
|------|-----|-------|-----------|---------------|
| `FixedLenBtree` | 固定长 | 固定长 | FIXED | FIXED |
| `VarKeySizeBtree` | 可变长 | 固定长 | VAR_KEY | VAR_KEY |
| `VarValueSizeBtree` | 固定长 | 可变长 | VAR_VALUE | FIXED |
| `VarObjSizeBtree` | 可变长 | 可变长 | VAR_OBJECT | VAR_OBJECT |
| `PrefixIntervalBtree` | Interval | Interval | PREFIX | FIXED |

**现状分析**：

`test_index_btree.cpp` 的 `BtreeTest`（短时单元测试）覆盖全部 5 种类型。但进入 long running 层面后差距极大：

- **`BtreeConcurrentTest`**（并发 long running）：`BtreeTypes` 注册了全部 5 种，但 `index_test.py` 的 `long_running()` 函数只调用 type=0（FixedLenBtree），type=1（PrefixIntervalBtree）被注释，type=2/3/4 从未出现在脚本中。

- **`IndexCrashTest`**（crash recovery 测试）：`BtreeTypes` 仅声明了 `FixedLenBtree` 和 `PrefixIntervalBtree` 两种，`VarKeySizeBtree`、`VarValueSizeBtree`、`VarObjSizeBtree` **根本不在 crash recovery 的类型列表中**，连短时 crash 单元测试也没有，更不用说 long running。

**影响**：

- `PrefixIntervalBtree`：有 PREFIX 节点格式和 interval 合并逻辑，其 split/merge 的 CP flush 路径与 FIXED 节点不同，crash recovery 路径未被任何 crash 测试验证。
- `VarKeySizeBtree` / `VarValueSizeBtree` / `VarObjSizeBtree`：可变长 key/value 使节点内的 slot 管理逻辑更复杂（需要 offset table），split 时的 key 边界计算与固定长节点完全不同，这些差异既没有 long running 并发测试，也完全没有 crash recovery 测试覆盖。

**建议**：

1. 调查 `PrefixIntervalBtree` 在 `long_running` 中被注释的原因，若已修复相关 bug，重新启用 type=1 的并发和 crash 测试。
2. 在 `IndexCrashTest` 的 `BtreeTypes` 中加入可变长类型（至少 `VarKeySizeBtree`），补充基础 crash recovery 单元测试。
3. 长期目标：将全部 5 种类型纳入 long running 测试，或明确说明哪些类型不需要 long running 覆盖及理由。

---

#### 差距 4：无并发 Crash 测试

**现状**：所有 crash recovery 测试（`IndexCrashTest`）均为单线程顺序操作（`m_is_multi_threaded = false` 或串行生成操作列表）。Crash 注入与操作执行之间没有并发。

**影响**：实际生产场景中，crash 发生时通常有多线程在并发执行操作。并发 + crash 的组合可能暴露单线程测试无法发现的竞态条件（如两个线程同时 split 同一个节点时 crash）。

**建议**：设计并发 crash 测试：启动多线程并发 put/remove，同时设置 crash flip，观察 crash 是否按预期触发，恢复后验证正确性。

---

#### 差距 5：缺乏 `update` 操作的 Crash 测试

**现状**：`long_running_crash_put` 和 `long_running_crash_remove` 只测试了 INSERT 和 DELETE。`update`（修改已有 key 的 value）操作未在任何 long running 场景中测试。

**影响**：update 操作在 B-tree 中是 "查找 + 修改 value" 的组合，其 CP flush 行为与 put 不同（node 内容更改但结构不变）。若 crash 发生在 update 操作完成但 CP 未落盘之前，恢复后 value 可能是旧值，这需要上层重新应用 update。

**建议**：在 crash_put_remove 的基础上加入 update 操作（使用 `btree_put_type::UPDATE` 或 `btree_put_type::UPSERT`），验证 update + crash 后 value 可正确恢复。

---

#### 差距 6：缺乏多 Index Table 的 Long Running 测试

**现状**：`IndexCrashTestTwoTables` 只有 `MultiTableMetaBufOrdinalCollisionOnRecovery` 这一个回归测试，没有长时间多表并发操作 + crash 的场景。

**影响**：多表共享 CP context 时，`txn_journal` 包含多个 ordinal 的记录，恢复逻辑的复杂度大幅提升。只有回归测试不足以覆盖长时间运行下的 corner case。

**建议**：增加 multi-table long running crash test：2~4 个 table 同时运行 put/remove，定期 crash，验证每个 table 恢复正确。

---

#### 差距 7：缺乏空间压力（Space Pressure）测试

**现状**：所有测试都在充裕空间的设备上运行（size_pct=70%，实际使用率远低于此）。没有在设备接近满的场景下测试 crash recovery。

**影响**：空间将满时，B-tree 的 block 分配行为可能不同（如找不到新 chunk 触发 `space_not_avail` 失败路径），这些路径的 crash safety 未被验证。range 操作在空间压力下更容易触发中途失败，shadow map 的"失败处理"策略（重试/重建）在此场景下的正确性也未被验证。

**建议**：在设备容量的 90%~95% 填充率下重跑 Crash Recovery 测试（方案 C/D/E），重点观察：`space_not_avail` 是否被正确向上传播（不被静默吞掉）；range 操作失败后，通过 range_query 重建 shadow map 的策略能否在高填充率下正常工作。

---

#### 差距 8：缺乏 CP 持久化的端到端验证（CP Semantics Explicit Test）

**现状**：现有测试隐式依赖 reapply 机制来处理"成功但未持久化"的操作，但没有显式测试：**"操作返回成功但 crash 后确实不存在"**这一语义。

**建议**：设计专门的 CP 语义验证测试：
1. 执行一批 put 操作（不触发 CP）
2. 注入 crash（不带 flip，直接 kill -9 模拟）
3. 重启后，**断言这些 put 的 key 不存在**（因为没有经过 CP flush）
4. 这确保了系统没有意外地"偷偷"持久化未 CP 的数据

---

#### 差距 9：Flip 轮换为顺序循环，缺少随机化

**现状**：`long_running_crash` 中 split flip 和 merge flip 各自按固定顺序循环（`cur_put_flip_idx++ % put_flips.size()`）。这意味着 flip A 永远出现在 flip B 之前，不同 flip 之间的组合顺序是固定的，无法测试 flip B 先触发、flip A 后触发的场景。

**影响**：某些 bug 只在特定 flip 先后组合下才会暴露。例如，连续两轮都触发 `crash_flush_on_split_at_parent` 然后触发 `crash_flush_on_merge_at_left_child` 的组合，与反过来的顺序可能走完全不同的恢复路径。顺序循环会系统性地错过一部分组合。

**建议**：在 long running 测试中对 flip 选取改为随机选取（`std::uniform_int_distribution` 从 flip 列表中随机选一个），flip 的选择结果同样记录到 `/tmp/flip_history.txt` 以便复现。

---

#### 差距 10：`save_mode` 默认关闭，操作序列未持久化

**现状**：`long_running_crash_options` 的 `save_mode` 字段默认值为 `SISL_OPTIONS.count("save_to_file") > 0`，即需要显式传入 `--save_to_file` 参数才会开启。当前 `index_test.py` 中没有传该参数，长时间测试实际上并未保存操作序列。

**影响**：测试失败时，只有种子可以用于复现。但种子依赖系统随机数实现，跨平台（不同 OS、不同 libc 版本）下同种子未必产生相同序列，导致在 CI 服务器上发现的 bug 无法在开发机上复现。

**建议**：在 `index_test.py` 的 `long_running_crash_*` 函数中，始终将 `save_to_file=True` 传入 `run_crash_test`（或通过 C++ 测试的默认参数开启），确保操作序列在任何环境下都被持久化。

---

#### 差距 11：缺乏 IO 错误注入测试

**现状**：所有 fault injection 均为进程 crash 模拟，没有任何测试在进程存活的情况下注入 write IO error 或 read IO error，错误处理路径完全未被验证。

**影响**：
- CP flush 写错误时，系统是否正确回退到上次 CP 状态、进程是否不崩溃——未验证，错误处理路径中可能存在隐患。
- B-tree 节点读错误时，操作是否返回正确的错误码而非静默失败——未验证，可能导致上层以为操作成功实则未执行。
- 错误处理路径在生产中极少执行，缺乏测试使其成为可靠性盲区。

**建议**：
1. 在 vdev/physical_dev 层增加 write/read IO error flip 注入能力
2. 新增 C++ 测试（方案 H）覆盖写错误和读错误两类场景
3. 长期将 IO 错误注入纳入 long running 测试循环（每 N 轮 crash 测试后插入一轮 IO 错误测试）
4. IO 错误测试同样应遵循两级可复现性策略：记录种子 + 保存注入序列

---

#### 差距 12：缺乏 CRC 不匹配数据完整性专项测试

**现状**：当前所有测试均假设磁盘数据完整，没有任何测试验证当 B-tree 节点 CRC 不匹配时系统的行为。

**影响**：静默数据损坏（silent corruption）是存储系统的真实威胁。若系统在 CRC 不匹配时未能正确返回 `crc_mismatch` 而是使用损坏数据继续运行，可能导致错误数据在树中扩散，且极难排查。

**建议**：实现方案 I，专项验证 `crc_mismatch` 错误路径的检测能力与错误隔离性。此为独立的数据完整性专项测试，不属于 long running 范畴。

---

#### 差距 13：`BtreeRemoveAnyRequest`（remove_any）未纳入测试设计

**现状**：`btree_test_helper.hpp:327` 已实现 `range_remove_any`（使用 `BtreeRemoveAnyRequest`），但 `test_index_btree.cpp` 的 long running 测试和 `IndexCrashTest` 的 crash recovery 测试均未使用该操作。

**影响**：remove_any 的 shadow map 更新逻辑与 single remove 和 range_remove 均不同——实际被删除的 key 不是调用方指定的，而是 B-tree 自主选择后通过 `out_key` 返回。若测试框架未正确读取 `out_key` 并同步更新 shadow map，验证逻辑将出现 false positive（shadow map 认为 key 存在，但树中已删除）。在 crash 场景下，若 remove_any 操作已部分落盘，reapply 逻辑也需要处理"out_key 所指 key 可能已不存在"的情况，与 single remove 的 reapply 策略相同（not_found 视为正常）。

**建议**：
1. 在 `BtreeConcurrentTest.ConcurrentAllOps`（方案 A）的操作集合中加入 remove_any，操作权重参照 remove。
2. 在 crash recovery 测试中（至少方案 E 混合读写场景），将 remove_any 作为第三种删除操作加入操作序列，重点验证 shadow map 通过 `out_key` 同步的正确性。

---

#### 差距 14：`TREE_TRAVERSAL_QUERY` 未用作 crash recovery 结构完整性验证工具

**现状**：`btree_req.hpp:192–196` 注释明确说明 `TREE_TRAVERSAL_QUERY` 的设计意图是"*used to check and recover if parent and leaf node are in different generations or crash recovery cases*"。但现有所有验证（包括 crash recovery 测试中的 `get_all()`）均使用默认的 `SWEEP_NON_INTRUSIVE_PAGINATION_QUERY`，`TREE_TRAVERSAL_QUERY` 在任何测试中均未出现。

**影响**：sweep query 沿叶节点链扫描，不回溯 parent；若 crash 后 parent 与 child 的代际（generation）不一致（parent 已落盘新版本但 child 尚未落盘，或反之），sweep query 可能正常返回结果而不报错，结构性不一致被掩盖。`TREE_TRAVERSAL_QUERY` 每次从 root 下沉到叶节点，能够检测到此类 parent/leaf 代际不匹配，是 crash recovery 后结构完整性检查的专用工具。

**建议**：在方案 C/D/E/F 的 crash recovery 流程中，reapply 并 CP flush 完成后，在执行 `get_all()`（sweep 模式）之前，额外对全量 key 范围执行一次 `TREE_TRAVERSAL_QUERY` 扫描，验证其返回结果与 shadow map 一致。若二者结果一致而 sweep 结果也一致，说明结构无误；若 `TREE_TRAVERSAL_QUERY` 报错或结果与 sweep 不一致，说明存在 parent/leaf 代际不一致 bug。

---

#### 差距 15：`btree_put_type`（INSERT / UPDATE / UPSERT）三种值的 crash 语义差异未分析

**现状**：计划文档将所有 put 操作统称为"put/update"，未区分 `btree_put_type` 的三种语义：
- `INSERT`：key 已存在则操作失败，树状态不变
- `UPDATE`：key 不存在则操作失败，树状态不变
- `UPSERT`：始终成功（除空间不足外），insert-or-update 语义

**影响**：shadow map 的更新策略和 crash 后 reapply 策略因 put_type 不同而存在差异：
- `INSERT` 失败（key 已存在）：shadow map 不更新，树不变——无特殊处理；但若 `INSERT` 成功后 crash，reapply 时必须改用 `UPSERT`（`force_upsert`），因为该 key 在 crash 前可能已部分落盘，再次 `INSERT` 会返回 key_found 失败。
- `UPDATE` 成功后 crash：key 本身已在 persisted snapshot 中，crash 不会令已持久化的 key 消失；但 reapply 时同样应统一使用 `UPSERT`（`force_upsert`）而非 `UPDATE`，原因：若同轮 diff 中有针对同一 key 的后续 REMOVE，reapply 顺序中 UPDATE 可能在 REMOVE 之后执行（若顺序计算有误），此时 key 已不存在，`UPDATE` 会失败；`UPSERT` 则始终安全且幂等，与 1.4 节的 reapply 策略保持一致。
- `UPDATE` 失败（key 不存在）：操作未被应用，shadow map 不更新。
- 当前 long running crash 测试使用的 put_type 未在文档中明确；建议统一规定：crash recovery 测试的 reapply 阶段，无论原操作为 INSERT、UPDATE 还是 UPSERT，一律使用 `force_upsert`（`UPSERT`）执行，确保幂等性。

**建议**：在测试设计文档中明确每类 crash 测试使用的 `btree_put_type`；在 shadow map 更新和 reapply 章节中，分别给出三种 put_type 对应的处理规则，确保 reapply 策略与操作类型匹配。

---

#### 差距 16：`SWEEP_INTRUSIVE_PAGINATION_QUERY` 在并发场景下的死锁风险未分析

**现状**：`btree_req.hpp:185–191` 对 `SWEEP_INTRUSIVE_PAGINATION_QUERY` 的注释明确警告："*retains the node and its lock during pagination … if the caller is not careful, the read lock will never be unlocked and could cause deadlocks*"。当前计划文档及测试（包括方案 A 并发测试）均未涉及该 query 类型。

**影响**：若有代码路径在持有该 query 的分页锁期间触发了 B-tree 写操作（put/remove/crash flush），则可能发生死锁。这类死锁在单线程测试中不可能出现，只有在并发测试中才能暴露。方案 A（`BtreeConcurrentTest.ConcurrentAllOps`）是当前唯一有并发读写的 long running 场景，但它使用的是默认的非侵入式 sweep query。

**建议**：在方案 A 的操作集合中加入少量 `SWEEP_INTRUSIVE_PAGINATION_QUERY`（低权重，如 2~5%），与并发 put/remove 交织运行，验证并发场景下不发生死锁。由于该 query 类型具有侵入性，建议在较小 batch_size 下测试以控制锁持有时间。

---

#### 差距 17：`BtreeGetAnyRequest`（get_any）未在 long running 或 crash 场景中测试

**现状**：`get_any` 仅在 `test_index_btree.cpp` 的两处短时基础测试中出现（测试成功/不存在两种返回），未进入任何 long running 或 crash recovery 测试。

**影响**：`get_any` 用于在 range 内获取任意一个 key，其正确性保证是"返回的 key 一定在 shadow map 对应 range 内存在"。在长时间并发操作和 crash 恢复场景下，若 B-tree 的 get_any 实现存在 race 或恢复后的 iteration bug，当前测试无法检测。

**建议**：在方案 A（`ConcurrentAllOps`）的操作集合中加入 `get_any`（低权重），验证逻辑为：对 `[start_k, end_k]` 执行 get_any，若 shadow map 中该 range 内存在 key，则断言返回的 `out_key` 在 shadow map 中存在；若 shadow map 中 range 为空，则断言返回 not_found。

---

### 2.3 差距汇总表

| 差距编号 | 描述 | 严重程度 | 当前状态 | 建议行动 |
|---------|------|---------|---------|---------|
| G1 | `crash_flush_on_root` 未在 long running 中测试 | 高 | 未覆盖 | 加入 put crash flip 列表 |
| G2 | `crash_flush_on_freed_child` 被注释 | 高 | 被注释跳过 | 调查原因，修复后启用 |
| G3 | 5 种 BTree 类型中仅 FixedLenBtree 有 long running；3 种 Var 类型无任何 crash 测试 | 高 | 严重不足 | 启用 PrefixInterval；Var 类型加入 IndexCrashTest |
| G4 | 无并发 Crash 测试 | 中 | 未实现 | 设计新测试 |
| G5 | `update` 操作未在 crash 场景中测试 | 中 | 未覆盖 | 扩展 crash_put_remove 测试 |
| G6 | 多 Index Table 无 long running crash 测试 | 中 | 只有回归测试 | 扩展 TwoTables 测试为 long running |
| G7 | 无空间压力场景测试 | 低 | 未实现 | 增加空间压力场景 |
| G8 | CP 持久化语义未显式验证 | 中 | 未实现 | 增加专项 CP 语义测试 |
| G9 | Flip 轮换为顺序循环，缺少随机化 | 低 | 顺序 cycle | 改为随机选取，结果记录到 /tmp/flip_history.txt |
| G10 | `save_mode` 默认关闭，操作序列未持久化 | 高 | 默认关闭 | index_test.py 中始终开启 save_to_file |
| G11 | 缺乏 IO 错误注入测试（写错误/读错误） | 中 | 未实现 | 新增 vdev 层 flip + 方案 H 测试 |
| G12 | 缺乏 CRC 不匹配数据完整性专项测试 | 低 | 未实现 | 实现方案 I（独立专项，非 long running） |
| G13 | `BtreeRemoveAnyRequest`（remove_any）未纳入测试 | 中 | 未覆盖 | 加入并发测试及 crash recovery 操作集合，正确处理 `out_key` shadow map 同步 |
| G14 | `TREE_TRAVERSAL_QUERY` 未用作 crash recovery 结构完整性检查 | 高 | 未使用 | crash recovery 后增加 TREE_TRAVERSAL_QUERY 验证 pass |
| G15 | `btree_put_type`（INSERT/UPDATE/UPSERT）crash 语义差异未分析 | 中 | 未区分 | 明确各 put_type 的 shadow map 更新与 reapply 策略 |
| G16 | `SWEEP_INTRUSIVE_PAGINATION_QUERY` 并发死锁风险未测试 | 中 | 未测试 | 加入方案 A 操作集合（低权重），验证并发下不发生死锁 |
| G17 | `BtreeGetAnyRequest`（get_any）未在 long running 或 crash 场景测试 | 低 | 仅短时测试 | 加入方案 A 操作集合，验证返回 key 在 shadow map 中存在 |

---

### 2.4 近期优先工作建议

按严重程度和实现难度排序，建议按以下顺序推进：

1. **（高优先）** 调查 `crash_flush_on_freed_child` 被注释的原因，确认是否有对应 bug，修复后在 `long_running_crash_remove` 中启用（G2）

2. **（高优先）** 在 `long_running_crash_put` 的 flip 列表中加入 `crash_flush_on_root`（G1）——代码改动极小

3. **（高优先）** 在 `index_test.py` 的 `long_running_crash_*` 函数中始终开启 `save_to_file`（G10）——代码改动极小（一行参数），但对可复现性的价值极高

4. **（高优先）** 调查 `PrefixIntervalBtree` long running 被注释的原因，评估是否可以启用（G3）；同时在 `IndexCrashTest` 的 `BtreeTypes` 中加入 `VarKeySizeBtree`，补充可变长类型的基础 crash 单元测试——3 种 Var 类型目前连 crash 单元测试都没有，风险较高

5. **（高优先）** 在方案 C/D/E/F 的 crash recovery 验证流程中，reapply + CP flush 完成后，增加 `TREE_TRAVERSAL_QUERY` 全量扫描（G14）——与 shadow map 比对，专门检测 crash 后 parent/leaf 代际不一致的结构性 bug；实现成本低（使用现有 query 接口，无需新增基础设施），但能覆盖 sweep query 遗漏的检测盲区

6. **（中优先）** 在 `long_running_crash_put_remove` 中加入 `update` 操作（G5）——需要在 `SequenceGenerator` 中增加 UPDATE 操作类型，以及对应的 reapply 逻辑

7. **（中优先）** 设计并发 Crash 测试（G4）——需要新增 C++ 测试类和 Python 调用入口

8. **（中优先）** 扩展 multi-table long running crash test（G6）

9. **（中优先）** 新增 vdev 层 IO 错误 flip 能力，实现方案 H 的写错误和读错误测试（G11）——需要基础设施改动，但能覆盖目前完全空白的错误处理路径

10. **（中优先）** CP 语义显式验证测试（G8）——验证"操作成功但 crash 后确实不存在"这一核心持久化语义，现有测试对此无覆盖

11. **（中优先）** 在测试设计文档和 shadow map 章节中明确 `btree_put_type` 三种值（INSERT/UPDATE/UPSERT）各自的 crash 语义与 reapply 策略（G15）；在 crash_put 测试中补充 INSERT-only 和 UPDATE-only 两种 put_type 的专项 round，验证 shadow map 更新逻辑与 reapply 的正确性

12. **（中优先）** 将 `remove_any`（G13）和 `get_any`（G17）加入方案 A（`ConcurrentAllOps`）的操作集合；将 `remove_any` 同时加入方案 E（混合读写 crash 测试），重点验证 `out_key` 的 shadow map 同步逻辑

13. **（中优先）** 在方案 A 中加入少量 `SWEEP_INTRUSIVE_PAGINATION_QUERY`（G16），与并发 put/remove 交织运行，验证并发场景下持锁分页不发生死锁

14. **（低优先）** Flip 轮换改为随机选取（G9）、空间压力测试（G7）

15. **（低优先）** 实现方案 I，验证 B-tree 节点 CRC 不匹配时的检测能力与错误隔离性（G12）——需要在读取路径增加 flip 注入点，是独立的数据完整性专项测试，不需要长时间运行
