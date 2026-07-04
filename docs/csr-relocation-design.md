# csr Phase 0 — 保 hop 的节点迁移(relocation)设计

## 背景与动机

`csr`(Cut-edge Reducer)目前有两个杠杆减少跨分区边:

- **Phase 1 resub**:改消费者的逻辑,用一个已经在消费者分区里的 divisor 替换跨分区的 fanin。零面积、零新增边,但需要这样的同分区 divisor 存在。
- **Phase 2 replicate**:把驱动者复制一份放到消费者分区。有面积代价,受 hop-slack + 节点增长上限门控。

两个杠杆都在改**消费者**或**驱动者**的逻辑,却从不问最简单的问题:**这个节点是不是干脆该待在另一个分区?**

实测数据(alu4/N4):`667 →(phase1) 573 →(phase2) 549`。两阶段合计只消除约 18% 的跨分区边,**82% 的边活了下来**——它们要么找不到同分区 divisor(phase1 打不动),要么无法在"严格下降 + hop-slack + 节点预算"下复制(phase2 打不动)。

这个 82% 里有一大类是结构性的:一个节点被它的多数邻居(fanin/fanout)"拉向"某个分区。hmetis 内部也做节点迁移,但它优化的是**平衡约束下的 cut-net**,停在自己受平衡限制的局部最优;一个**以 cut-edge 为目标、放松平衡约束**的迁移 pass 能探索 hmetis 结构性拒绝的迁移。

本设计新增第三个杠杆:**纯重新标记的节点迁移**(只改 `part_id`,零面积、零逻辑改动)。

## 约束边界(硬/软)

来自需求澄清,明确区分:

- **硬约束(不可违反):hop 数不能变差。** 每次迁移后网络的 max hop(`Abc_NtkComputeHopNum`)不得超过迁移前的基线。
- **硬约束:分区失衡有可控上限。** 不能把节点无限制地挪进同一个分区(否则退化成"全挪到一个分区")。复用现有平衡百分比机制设上限。
- **软约束(可接受):** 面积增长(本阶段实际为零)、分区适度失衡(在上限内)、运行时间变慢(可接受更精确但更慢的检查)。

## 算法

### 定位:新增 Phase 0,放在最前

新增 `run_phase0_relocate`,在现有 phase1/phase2 **之前**运行。管线变为:

```
relocate → resub → replicate
```

理由:

1. 迁移是三个杠杆里最便宜的(零面积、零逻辑),应优先用它把分区边界结构性收紧。
2. 迁移改变分区归属后,phase1 可能因此找到原本没有的"同分区 divisor"——先迁移能给后续阶段创造机会。

### 迁移收益与目标分区选择

节点 `n` 当前在分区 `A`。候选目标分区**只取 n 的邻居(fanin + fanout,仅 part_stat 顶点)出现过的分区**——迁到没有邻居的分区只会增加跨分区边,无意义。

定义节点 `n` 的**关联跨分区边数** = 它所有 fanin 边 + 所有 fanout 边中跨分区的条数(仅统计 part_stat 顶点:PI / 内部 NODE / CONST1;PO 不计,与 `Abc_NtkComputeCutEdgeNum` / `ComputeCutEdgeCount` 的约定一致)。

对每个候选目标 `P`:

```
delta(n, A→P) = incident_cross_edges(n | n在P) − incident_cross_edges(n | n在A)
```

取使 `delta` 最小的 `P`。**只接受 `delta < 0`(严格净减少)的迁移。** 严格下降保证全局 cut-edge 单调下降、循环必然收敛、不振荡。

注意:一次迁移只改 `n` 自己的 `part_id`,因此只翻转"`n` 的 fanin 边"和"`n` 的 fanout 边"的跨分区状态,不影响任何其他边。所以 `delta` 是**局部可算**的(O(fanin + fanout)),等于该迁移对全局 cut-edge 的精确增量。

### hop 门槛(精确全局重算)

利用"运行时间不敏感"的边界,采用**精确全局重算**而非静态 slack 快照(与 Phase 2 的静态快照策略不同,理由见下):

```
relocate 开始时: baseline_max_hop = Abc_NtkComputeHopNum(pNtk)
对每个通过 delta<0 与平衡门槛的候选迁移:
    改 part_id: A → P
    new_max_hop = Abc_NtkComputeHopNum(pNtk)
    if new_max_hop > baseline_max_hop:
        回滚 part_id: P → A   (拒绝该迁移)
    else:
        接受(baseline_max_hop 保持不变,因为 new_max_hop <= baseline)
```

`Abc_NtkComputeHopNum` 已存在(就是 `ps` 输出里的 `hop` 值),O(nodes)。因为只有通过便宜的 `delta<0` 和平衡检查的候选才会触发全局重算,触发次数少。

**为什么用精确全局重算而非 Phase 2 式静态 slack:**

- Phase 2 复制只改复制品自己 fanin 边的跨分区状态,局部 O(fanins) 检查就够,静态快照是合理的性能取舍。
- 迁移不同:它同时翻转节点的 fanin 边**和所有 fanout 边**的跨分区状态,对下游 hop 的影响更广,静态快照更容易失真。既然运行时间不敏感,直接精确重算最准——只拒绝真正让 hop 变差的迁移,不误杀。
- `baseline_max_hop` 保持为迁移前的初值(接受的迁移满足 `new_max_hop <= baseline`,所以基线不需要更新;这样保证整个 relocate 阶段 hop 相对进入时的值永不变差)。

### 平衡门槛(可控上限)

复用 cpr 已公开的平衡原语(`src/cpr/cpr.hpp`):

```cpp
void partition_sizes(Abc_Ntk_t *pNtk, int num_parts, std::vector<int> &sz);
int  compute_balance_max_allowed(const std::vector<int> &sz, int balance_pct);
```

流程:

```
relocate 开始时: partition_sizes(pNtk, num_parts, sz)
                max_allowed = compute_balance_max_allowed(sz, balance_pct)
迁移 n: A→P 前检查:  sz[P] + 1 <= max_allowed  ?
    是 → 允许(接受后 sz[P]++, sz[A]--,增量维护)
    否 → 跳过该迁移(该分区已达上限)
```

`balance_pct` 复用现有 `-B` flag(默认继承 pdb 的平衡度,再兜底到 2,与 phase2/enforce_balance 的取值逻辑一致)。`max_allowed` 在 relocate 开始时算一次;`sz[]` 随每次接受的迁移增量更新。这样即使大量节点都想挤进同一分区,一旦该分区达到 `max_allowed` 就拒绝后续迁入,**从机制上杜绝"全挪到一个分区"**。

### 循环结构(FM 式多轮)

```
for round in 0..max_rounds:
    收集所有节点的当前最优迁移(delta<0 的),按 |delta| 降序(收益大的先做)
    若没有候选 → break
    round_moved = 0
    for each 候选 (按收益降序):
        重新核对该节点当前最优迁移(邻居可能已被本轮先前的迁移改动,delta 过期就重算/跳过)
        若仍 delta<0 且过平衡门槛且过 hop 门槛:
            接受迁移,round_moved++
    若 round_moved == 0 → stall_count++; 若 stall_count >= stall_limit → break
    否则 stall_count = 0
```

复用现有 `-R`(max_rounds)/`-S`(stall_limit)配置。"应用前重新核对"是必须的:贪心地按预先算好的收益排序应用时,先前的迁移会改变邻居的跨分区状态,导致后面候选的 `delta` 过期——所以每个候选在真正应用前用当前 `part_id` 重算 `delta`,过期(不再 `<0`)就跳过。

## 命令接口

新增一个开关控制 relocation 是否运行,默认**开启**(它是零面积、纯收益的阶段)。具体 flag 字母在实现时定(现有已占用:`-R -S -X -G -B -b -v`)。倾向用一个"关闭 relocation"的开关(如 `-L` 之类),保持默认全开。

`-B`(平衡百分比)现在同时被 relocation 的平衡门槛和(可选的)enforce_balance 复用。

## 文件结构

改动集中在:

- `src/csr/csr.cpp`:新增 `run_phase0_relocate` 及其辅助(候选收集、`delta` 计算、hop 门槛、平衡门槛),在 `ApplyCsr` 里 phase1 之前调用。复用已有的 `is_part_stat_vertex`、`ComputeCutEdgeCount`、`resolve_num_parts`。
- `src/csr/csr.hpp`:如需新增 Config 字段(如 `do_relocate`)则在此加。
- `src/main.cpp`:`Csr_Command` 加新 flag 解析 + usage 更新。
- `src/cpr/cpr.hpp` / `cpr.cpp`:平衡原语已公开,无需改动,直接链接复用。

## 测试与验收

用已建好的冻结分区 baseline 消除 hmetis 随机性:

- 分区来源:`hpart --load-part regression/parts/N{4,16}/<case>.part`(由 `scripts/run_hpart_baseline.py` 冻结,`regression/hpart_baseline.json` 记录基线指标)。
- 用 `scripts/run_csr_regression.py`(需先适配 `ps` 新输出标签 `cut-net =`/`cut-edge =`,见下)在 90 个 SimpleCircuits case 上对比 **relocation 开启前 vs 开启后** 的 cut-edge / hop / area,并跑 `--cec` 验证功能等价。

**验收标准:**

1. **功能正确性:** 全部 case `cec` = EQ。迁移是纯 `part_id` 重标记,逻辑不变,cec 理论上必然 EQ——若出现 NOT_EQ 说明实现有 bug。
2. **hop 硬约束:** 每个 case relocation 后的 hop **不大于** relocation 前的 hop(逐 case 核对,不是总量)。
3. **cut-edge 收益:** 总 cut-edge 相对"仅 phase1+phase2"进一步下降;开启 relocation 的 case 里有净收益的比例 ≥ 现状(70/90)。
4. **平衡上限:** 各分区节点数不超过 `max_allowed`(逐 case 核对 pmax)。
5. **无回归:** 现有 phase1/phase2 行为不受影响(relocation 可通过 flag 关闭,关闭时结果与当前 `main` 完全一致)。

## 已知依赖 / 顺带修复

- **`ps` 输出标签变更**:`786a8ef` 已把 `ps` 的 `cut =` 拆成 `cut-net =` / `cut-edge =`。`scripts/run_csr_regression.py` 的 `CUTNET_RE = \bcut =` 正则不再匹配,需在测试前更新为匹配 `cut-net =`(cut-net)并可选新增匹配 `cut-edge =`。这是本任务测试链路的前置修复,纳入实现范围。

## 后续方向(本设计不含)

- **方向 C(不动点循环)**:待 relocation 单独验证有效后,再考虑把 `relocate → resub → replicate` 三杠杆循环到不动点(互相解锁:迁移可为邻居创造同分区 divisor,复制可让原本持平的迁移变得有收益)。本设计只做独立的 Phase 0,为 C 去风险。
