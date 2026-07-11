# CSR 稳健型优化增强设计

## 1. 目标与范围

本设计在 `src/csr/` 内部增强现有 CSR，不调用或编排 `cpr`、`cmfs`、`rewrite`、重新映射或重新分区。目标是在保持现有稳健性约束的前提下，进一步降低跨分区 cut-edge。

设计采用“强化现有 Phase 0/1/2 + 轻量确定性多轨迹搜索”的路线，不引入全局 ILP/SAT 分区优化。

### 1.1 硬约束

以 CSR 入口网络为基线，整个优化过程必须满足：

- 最大跨分区 hop 不增加；
- 当前存活节点数不超过入口节点数的 102%；
- Phase 1/2 累计消耗的净新增节点额度不超过入口节点数的 2%，后续删除节点不返还额度；
- cut-net 不超过入口 cut-net 的 150%；
- 每次提交的普通候选必须严格降低 cut-edge；
- Phase 0 短序列允许中间步骤不降低 cut-edge，但整个序列提交时必须严格降低；
- 保持逻辑功能等价；
- 默认 `-T 1` 配置的几何平均运行时间不超过当前 CSR 的 5 倍。

对应预算在入口统一冻结：

```text
hop_limit     = entry_hop
node_limit    = ceil(entry_nodes * 1.02)
growth_budget = floor(entry_nodes * 0.02)
cutnet_limit  = ceil(entry_cutnet * 1.50)
```

### 1.2 非目标

- 不修改 `hpart` 的划分结果；
- 不把 cut-net 改为首要优化目标；
- 不允许用 hop 或面积恶化换取 cut-edge 收益；
- 不实现大窗口全局精确求解器；
- 不把本设计扩展成 `csr/cpr/cmfs` 联合流水线。

## 2. 总体架构

CSR 保留现有阶段顺序，在外层增加确定性的多轨迹搜索：

```text
入口网络
  ├─ 轨迹 0：确定性收益优先
  ├─ 轨迹 1：收益与边界集中优先
  └─ 轨迹 2：稀缺机会优先
       每条轨迹执行增强 Phase 0 → Phase 1 → Phase 2
  → 从合法结果中选择 cut-edge 最少的网络
```

建议为命令增加 `-T num`，范围 1–3，默认 1。`-T 2/3` 是显式开启的额外局部最优搜索，不属于默认 5 倍运行时间验收范围，但仍受确定的操作次数预算和单案例超时限制。

每条轨迹从 CSR 入口网络的独立 `Abc_NtkDup` 快照开始。ABC 网络复制会通过 `Abc_ObjSetPartId` 重建对象 `part_id` 和 `pPdb`，但不会保留 `Pdb` 的 `num_parts`、`balance_pct`、缓存 cut-size 或缓存 hop 等标量状态。因此这些入口标量必须在第一次复制或修改网络之前保存到 `OptimizationState`，后续阶段不得重新从已修改或复制的 `pPdb` 读取它们。

`OptimizationState` 是分区数和平衡参数的权威来源。入口 `num_parts` 无效时通过对象 `part_id` 扫描解析；入口 `balance_pct` 无效时只在入口处回退为 2%，之后固定使用解析后的值。安装最终获胜网络后，使用全网重算结果调用 `Abc_NtkSetPartStats`，并把解析后的入口 `balance_pct` 写回获胜网络的 `pPdb`，恢复对后续命令可见的元数据。

最终结果采用以下字典序选择：

1. cut-edge 更少；
2. cut-net 更少；
3. hop 更少；
4. 节点数更少；
5. 轨迹编号更小。

轨迹排序不使用随机数。所有相同收益的候选最终使用对象 ID、分区 ID 和轨迹编号作为 tie-break，禁止依赖指针地址、散列表遍历顺序或未定义的 `std::sort` 等价元素顺序。

## 3. 统一优化状态

新增 CSR 内部 `OptimizationState`，由三个 Phase 共享。它负责维护：

- 入口和当前的 cut-edge、cut-net、hop、节点数；
- 各分区当前大小和允许上限；
- 当前 hop-arrival 状态；
- 单调累计的节点增长消耗和剩余额度；
- 当前轨迹策略；
- 入口 `num_parts` 和 `balance_pct`；
- 轮初网络快照和操作日志。

所有候选使用两级检查：

1. **快速预测**：通过局部 cut-edge delta、受影响 cut-net、fanin 集合、hop-arrival 和节点增量拒绝明显非法的候选；
2. **精确验证**：对可逆操作暂时应用，再核对真实指标；对 MFS resub 则根据旧、新 fanin 集合精确计算受影响 net 的 cut-net 状态，并在轮末执行全网审计。

relocation、swap 和 replication 等可逆操作在暂时应用后，必须调用 `Abc_NtkComputeCutSize` 精确检查 cut-net，合法后才能提交。Phase 1 使用假设 fanout 集合做逐提交的精确局部判断，轮末全网重算只是实现审计，不是允许本轮暂时超过预算的延迟门槛。

每轮结束统一执行：

- 全网重算 cut-edge；
- 使用 `Abc_NtkComputeCutSize` 全网重算 cut-net；
- 全网重算 hop；
- 检查节点数、分区大小和网络结构；
- 核对增量状态与全网结果。

若审计失败，恢复轮初快照并停止当前轨迹。轨迹失败不影响其他轨迹；全部轨迹失败时保持 CSR 入口网络不变并返回失败。

### 3.1 确定的搜索工作量预算

为了同时满足确定性和默认 5 倍运行时间约束，不使用墙钟时间决定搜索停止点。默认单轨迹采用以下操作次数上限：

- Phase 0：每轮最多扩展 512 个 beam 状态；
- Phase 1：每个节点最多评估 32 个 divisor 集合、执行 128 次 `Abc_NtkMfsTryResubOnce` 调用、保留 4 个 SAT 成功方案；
- Phase 2：每个 `(driver, target_partition)` 最多评估 16 个复制簇；
- 任一预算耗尽时按确定性候选顺序停止当前节点或当前候选的扩展，不影响已提交结果。

这些上限是功能设计的一部分。实现后通过消融和运行时间回归调整时，只能降低默认上限；提高上限需要重新验证 5 倍运行时间标准。

`growth_budget` 是新增节点操作的主要门槛；在“不返还额度”的语义下，`node_limit` 理论上不会被突破，因此它作为独立断言和防御性检查保留，避免计数实现错误。

## 4. Phase 0：短序列分区迁移

### 4.1 保留现有操作

继续执行现有：

- 单节点 relocation；
- 相邻跨分区节点 swap。

它们仍要求 cut-edge 严格下降、hop 不超过入口基线，并遵守分区平衡和 cut-net 预算。

### 4.2 新增短序列搜索

单节点 relocation 和 swap 收敛后，启动深度受限的迁移序列搜索。它统一覆盖：

- 两个节点共同迁入邻居分区；
- 第一步本身无收益、但为第二步创造收益的迁移；
- 三节点分区循环；
- swap 无法表达的非对称组合迁移。

搜索限制为：

- 每轮从边界选择最多 64 个种子节点；
- 每个节点只考虑 fanin/fanout 已出现的邻居分区；
- 最大搜索深度为 3；
- beam width 固定为 8；
- 使用局部 cut-edge delta 进行快速剪枝；
- 只对 beam 最终保留的候选做精确全网检查。

一个序列可以包含暂时不降低 cut-edge 的中间步骤，但中间状态仍不得违反 hop、cut-net 和分区平衡硬约束。整个序列只有在最终 cut-edge 严格下降时才原子提交。中间状态也必须满足 hop 是有意采用的保守限制：它会放弃“暂时升 hop、最终恢复”的序列，以换取简单且可验证的安全性。

Phase 0 只修改 `part_id`，不创建逻辑节点，因此不消耗节点增长额度。

### 4.3 轨迹差异

- 确定性收益优先：接近当前候选收益排序，但使用完整稳定 tie-break；它不是旧 CSR 的位级兼容模式；
- 收益/边界集中：优先局部 cut-edge delta 大、同一 driver 跨区 fanout 集中的区域；
- 稀缺机会：优先可选目标分区少、容易被其他迁移破坏的节点。

## 5. Phase 1：节点级多方案 resub

### 5.1 候选粒度

Phase 1 从逐条 `(consumer, iFanin)` 修复改为以 consumer 节点为基本候选。对每个边界节点收集全部跨分区 fanin，并在一个 MFS 窗口内比较多个等价实现。

每个 divisor 记录：

- 所在分区；
- hop arrival；
- SAT counterexample 覆盖能力；
- 加入新 fanin 集后的 cut-edge 变化；
- 受影响 driver 的 cut-net 变化；
- 与节点现有 fanin 的结构关系。

### 5.2 搜索层次

按成本从低到高尝试：

1. 删除一个跨分区 fanin；
2. 联合删除两个跨分区 fanin；
3. 用一个 divisor 替换一个或两个跨分区 fanin；
4. 用两个 divisor 联合替换；
5. `-X` 开启时，尝试多 divisor 和 Shannon 分解。

允许使用外分区 divisor，但必须使 consumer 的跨分区 fanin 总数严格下降。例如，允许用一个外分区 divisor 联合替换两个原有跨分区 fanin，实现两条 cut-edge 换一条 cut-edge。

### 5.3 Divisor 排序与组合

不再简单使用无序的同分区 divisor 或固定截断前 30 个。先按以下信息排序：

- counterexample 覆盖率；
- 预测 cut-edge 收益；
- cut-net 增量；
- hop 余量；
- 结构互补性。

保留最多 64 个高价值 divisor。双 divisor 只生成 counterexample 覆盖互补的组合，再由统一 Phase 1 工作量预算选择最多 32 个 divisor 集合进行实际 SAT 评估，不做完整平方级排列。

### 5.4 多成功方案选择

Phase 1 不在第一个 SAT 成功方案出现时立即更新网络，而是每个节点最多收集 4 个 SAT 成功方案，按以下顺序选取：

```text
cut-edge 降幅
→ cut-net 增量更小
→ hop 余量更大
→ 新 fanin 更少
→ divisor ID
```

每个节点每轮最多提交一个实现，避免连续修改同一节点导致 MFS 窗口和对象 ID 失效。

提交前从新 fanin 集合计算 cut-edge、受影响 cut-net、hop arrival 和 Shannon 分解节点增量。受影响 cut-net 的计算必须覆盖所有被删除的旧 fanin driver 和所有新增 divisor driver，并判断它们在假设改接后的完整 fanout 分区集合；只有预测结果不超过 `cutnet_limit` 才能提交。Phase 1 每轮保存网络快照；若提交后的 `Abc_NtkComputeCutSize` 审计发现实现错误，停止并销毁当前 MFS manager，恢复整轮、拉黑相关候选，再从新网络对象重建下一轮全部 MFS 状态。

## 6. Phase 2：目标分区聚合与复制簇

### 6.1 聚合候选

复制候选改为 `(driver, target_partition)`，而不是单条 cut-edge。每个候选预先计算：

- 可消除的 driver 到目标分区 fanout cut-edge 数；
- 复制品 fanin 新增的跨区边数；
- cut-net 变化；
- 节点成本；
- hop 风险。

候选优先级为：

```text
净 cut-edge 收益 / 新增节点数
→ 总 cut-edge 收益
→ cut-net 增量更小
→ hop 风险更低
→ driver ID / target partition ID
```

### 6.2 复制簇

当单独复制 driver 不能降低 cut-edge 时，允许沿 fanin 方向构造小型复制簇：

- 最大深度 2；
- 最多复制 3 个普通逻辑节点；
- 只扩展会减少复制品跨区输入的 fanin；
- 所有复制品放入同一目标分区；
- 按拓扑顺序创建复制品并连接簇内副本；
- 复制簇内部边全部是同分区边，不计入 cut-edge/cut-net；增量收益只统计复制簇边界上的原 fanout 和外部 fanin；
- 整个复制簇原子提交或回滚。

PI 和常量仍禁止复制。

### 6.3 增量 HopState

Phase 2 不再在整个阶段永久复用入口时的静态 hop-slack。维护可增量更新的 `HopState`：

1. Phase 2 开始时通过 `Abc_NtkDfs` 建立 part-stat 节点的拓扑序号，并完整计算初始 arrival；
2. 暂时接入复制簇，先根据复制品的全部 fanin 计算复制品 arrival，再改接目标 fanout；
3. 每个复制品继承对应原节点的拓扑序号，复制簇按原节点拓扑序创建，并断言复制品序号小于所有被改接 fanout 的序号；断言不成立时放弃增量路径并重建全网 DFS 顺序；
4. 将被改接的 fanout 放入按 `(拓扑序号, obj_id)` 排序的工作队列；
5. 每次弹出节点时，按照 `Abc_NtkComputeHopNum` 的定义，对它的全部 part-stat fanin 重新取最大值，而不是只检查发生变化的输入边；
6. 节点 arrival 发生变化时，只在第一次变化时记录 `(obj_id, old_arrival)`，并把它的全部 part-stat fanout 加入队列；未变化时停止沿该节点传播；
7. 传播同时覆盖 arrival 增加和降低，直到工作队列为空；任一节点超过 `hop_limit` 时，逆序恢复 arrival 日志和复制拓扑；
8. 接受后保留更新后的 HopState。

上述传播必须在受影响 TFO 上得到与全网 `Abc_NtkComputeHopNum` 完全一致的结果。每轮结束仍全网重算 hop 作为实现审计；不一致时恢复轮初网络。

Phase 1 Shannon 分解和 Phase 2 复制共享统一节点增长预算。每次操作只把该操作造成的正净增长计入 `growth_used`；后续操作即使删除节点也不返还已经消耗的额度。达到 `growth_budget` 或 `node_limit` 后，不再接受任何新增节点的候选。

## 7. 事务与错误处理

公共入口改为接收 `Abc_Frame_t *`，由 CSR 顶层负责网络所有权。入口网络在所有轨迹运行期间保持不变；每条轨迹只修改自己的副本。最终通过 `Abc_FrameReplaceCurrentNetwork` 安装获胜网络并释放入口网络，其他轨迹网络全部显式删除。全部轨迹失败时删除所有副本，frame 中的入口网络保持不变。

轨迹内部需要恢复轮初快照时，优化函数持有可替换的 `Abc_Ntk_t *`。它先销毁引用旧网络对象的 MFS manager、divisor 向量和其他轮内状态，再删除失败网络并把轨迹指针替换为快照；任何旧对象指针不得跨恢复继续使用。

各操作的回滚方式为：

- relocation/swap：记录旧 `part_id`；
- Phase 0 短序列：记录按执行顺序排列的全部 `part_id` 修改，逆序恢复；
- replication：记录新建节点及每个 `Abc_ObjPatchFanin` 的旧、新 fanin；
- HopState：记录本次传播修改过的对象 ID 和旧 arrival；
- MFS resub：依靠轮初网络快照处理无法安全逆向恢复的更新，恢复前必须先销毁 MFS manager。

候选失败属于正常搜索结果，不输出错误。只有以下情况终止当前轨迹：

- 全网审计与增量状态不一致；
- 网络结构检查失败；
- 快照或网络复制失败；
- MFS 更新后无法恢复合法逻辑网络。

verbose 输出应区分候选拒绝、轮回滚和轨迹失败，便于回归定位。

## 8. 命令接口与统计

保留现有 CSR 参数，新增：

```text
-T num  : deterministic search trajectories, 1-3 [default = 1]
```

现有 `-G` 节点增长百分比继续保留，但其预算改为相对于 CSR 入口节点数，并由 Phase 1/2 共享。预算按累计正净增长单调消耗，删除节点不返还额度。默认仍为 2%。这是有意改变的预算语义：Phase 1 Shannon 分解可能消耗全部额度并使 Phase 2 无法复制。轨迹 0 只保证确定的候选顺序，不保证与旧 CSR 输出一致。

最终输出增加：

- 每条轨迹的 cut-edge、cut-net、hop、节点数和运行时间；
- 最终选择的轨迹；
- Phase 0 单 move、swap、短序列次数；
- Phase 1 单删除、联合删除、单/双/multi-divisor 成功次数；
- Phase 2 单节点复制和复制簇次数；
- 各硬约束导致的拒绝次数。

## 9. 测试设计

### 9.1 定向小案例

增加小型回归网络，分别覆盖：

- 单 move 无收益、两步迁移有收益；
- 三节点分区循环；
- 两条跨区 fanin 被一个外区 divisor 替换；
- 第一个 SAT 成功方案不是最佳方案；
- 单节点复制无收益、复制簇有收益；
- 增量 hop 传播触发回滚；
- 增量 HopState 在 arrival 增加和降低时均与全网重算逐节点一致；
- cut-net 达到 150% 后拒绝候选；
- 同一轮多个 Phase 1 提交接近 cut-net 上限时，逐提交预测不得越过 150%；
- 节点增长达到 2% 后拒绝新增节点；
- Phase 1 用尽共享增长额度后，Phase 2 必须稳定地拒绝所有复制候选；
- 使用非 2% 的 `hpart -B` 冻结分区时，所有轨迹使用入口平衡参数而不是 Pdb 失效后的默认值。

每个案例检查目标 Phase 的操作计数、最终指标和功能等价性。

### 9.2 冻结分区全量回归

使用现有 MCNC、EPFL、OpenCores 和 VTR 的冻结分区文件，对比当前 CSR 与增强 CSR。每个跑通案例检查：

- `cec` 等价；
- hop 不超过入口值；
- 节点数不超过入口的 102%；
- cut-net 不超过入口的 150%；
- cut-edge 不高于入口；
- 相同输入重复运行 3 次，最终指标和阶段统计完全一致；
- 三轨迹大型案例在 ASan 下无快照泄漏、double-free 或恢复后的悬空 MFS 指针。

分别关闭短序列迁移、联合 resub、复制簇和多轨迹，执行消融实验，记录每项机制的独立收益和运行时间。

### 9.3 验收标准

- 全部跑通案例的最终 cut-edge 总和，相比当前 CSR 至少再降低 3%；
- 至少 60% 的案例不差于当前 CSR；
- 对当前 CSR 已满足新约束的案例，增强版不得恶化超过 1%；
- 默认 `-T 1` 的几何平均运行时间不超过当前 CSR 的 5 倍；
- 单个大型案例超时设为 `max(30 秒, min(当前 CSR 用时的 8 倍, 600 秒))`；
- `-T 2/3` 单独报告运行时间，不作为默认配置验收结果；
- 0 个 hop、节点数、cut-net 或功能等价性违规；
- 冻结分区下连续 3 次结果完全确定。

若完整方案未达到 3% 总体额外收益，则不默认开启没有稳定收益的复杂机制。根据消融结果，只保留满足约束且能稳定贡献收益的部分。

## 10. 实施顺序建议

后续实现计划应按以下风险顺序拆分：

1. 统一 `OptimizationState`、入口 Pdb 标量、指标口径和确定性排序；
2. 单轨迹回归，验证默认工作量预算、快照所有权和基础行为；
3. Phase 2 聚合候选和动态 HopState；
4. Phase 1 divisor 排序与多成功方案选择；
5. Phase 1 联合 fanin 替换；
6. Phase 0 短序列 beam 搜索；
7. Phase 2 复制簇；
8. 多轨迹外层、全量回归和消融实验。

先建立统一约束和确定性基础，再引入组合搜索，便于每一步独立验证和定位收益来源。
