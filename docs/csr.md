# csr — Cut-Edge Reducer

## 背景

`hpart` 对映射后的网表做 hmetis 划分后，跨分区边（cutsize）是后续物理实现代价的直接来源。FoxSYN 已有两个跟分区打交道的优化命令：

- **`cpr`**：relocate / replicate 节点以减少 **hop 数**（timing），cutsize 只是一个软约束上限，不是优化目标。
- **`cmfs`**：SAT-based resub 优化关键路径 **arrival**（timing），同样不直接针对 cutsize。

`csr`（Cut-edge Reducer）填补的是第三个目标：**直接把跨分区边数量作为优化对象**，不管 timing。适合在 `hpart` 之后、`cpr`/`cmfs` 之前或之后独立调用。

### cut-edge vs cut-net

`hpart`/`Pdb::cut_size()` 统计的是 **cut-net**：一条 net 只要有任意一个 fanout 跨分区，driver 就记一次，不管有几个 fanout 跨分区。

`csr` 优化的是 **cut-edge**：每一对跨分区的 `(driver, consumer)` 都单独计数。一条 net 最多贡献 1 条 cut-net，但可以贡献多条 cut-edge。

选 cut-edge 作为优化目标是因为它跟"每次修复的收益"粒度一致——resub 或 replication 每次只能消除某个 consumer 到某个 driver 的一条边，用 cut-edge 计数能让"贡献值"和"实际修复动作"对应起来；用 cut-net 计数会出现「明明修了一条边，net 却因为还有别的 fanout 跨分区而没降」的失真。

## 算法流程

`csr` 是 **resub-first + replication-fallback** 两阶段设计：

```
ApplyCsr(pNtk, cfg):
    initial_cutedges = ComputeCutEdgeCount(pNtk)

    Phase 0: run_phase0_relocate   -- 保 hop 的节点迁移(纯重标记,零面积),默认开启,-L 关闭
    after_phase0 = ComputeCutEdgeCount(pNtk)

    Phase 1: run_phase1_resub      -- 优先尝试不增加节点的 SAT resub
    after_phase1 = ComputeCutEdgeCount(pNtk)

    Phase 2: run_phase2_replicate  -- resub 打不动的边，用节点复制兜底
    after_phase2 = ComputeCutEdgeCount(pNtk)

    if cfg.do_balance_repair (-b):
        cpr::enforce_balance(...)  -- 可选的收尾分区平衡修复，默认关闭

    final_cutedges = ComputeCutEdgeCount(pNtk)
```

### Phase 0：保 hop 的节点迁移(relocation)

Phase 1/2 都在改消费者或驱动者的逻辑,却不处理"节点本身应该待在别的分区"这类结构性跨分区边。Phase 0 补上这个杠杆:对每个节点,计算迁到某个**邻居分区**(fanin/fanout 出现过的分区)后自身关联跨分区边(fanin 边 + NODE fanout 边,PO 不计,与 `ComputeCutEdgeCount` 同约定)的变化 `delta`,贪心地应用 `delta < 0` 的迁移。纯 `part_id` 重标记,零面积、零逻辑改动。

三重门槛,缺一不可:

1. **cut-edge 严格下降**:只接受 `delta < 0`,且在真正应用前用当前分区重算 `delta`(FM 式多轮里前面的迁移会让预算过期)。
2. **hop 精确不变差**:每次试探性迁移后重算全网 `Abc_NtkComputeHopNum`,超过 Phase 0 进入时的基线就回滚。区别于 Phase 2 的静态 slack 快照——迁移会同时翻转节点 fanin 边和所有 fanout 边的跨分区状态,影响更广,精确重算最稳(运行时间不敏感,可承受)。
3. **平衡上限**:复用 cpr 的 `compute_balance_max_allowed`,分区节点数达上限就拒绝迁入,从机制上杜绝"全挪到一个分区"。

Phase 0 放最前:先用零成本迁移收紧边界,还可能给 Phase 1 创造出原本没有的"同分区 divisor"。`-L` 关闭(默认开启)。

**swap 子循环**:单节点迁移收敛后,Phase 0 再跑一个 swap 子循环,补单节点贪心的盲区——两个相邻节点分属不同分区,单独迁任一个都不下降(各自还有邻居把它拉住),但**同时互换分区**能让它们之间那条边消失、加总 cut-edge 下降。候选只取互为 fanin/fanout、且分属不同分区的 NODE 对(候选数正比于跨分区边数,非 O(N²);非相邻对的 swap 恒等于两个独立单节点迁移,已被前一循环覆盖)。收益判据用 `ComputeCutEdgeCount` 前后差(与 Phase 2 同,规避 A–B 边重复计数);hop 门槛同单节点(互换后全网重算不超过 swap 子循环入口基线,否则回滚两个 part_id)。**swap 天然保平衡**(两分区各 -1+1=0),无需平衡门槛——这是它相对单节点迁移的优势。同属 `-L` 开关。

**副作用与 Phase 1 hop 门槛**:relocation 重塑分区布局后,暴露出 Phase 1 resub 原本没有 hop 门槛的缺陷——把一个跨分区但 hop 浅的 fanin 换成同分区但 hop 深的 divisor,边不跨区了(cut-edge↓)但消费者的 hop_arrival 反而上升(实测 arbiter 4→5)。修复:Phase 1 每轮开头对全网算一次 hop_arrival 快照,single/double-divisor resub 提交前预检新节点的预期 hop_arrival,超过该节点快照预算就放弃这条 resub。数学上 sound:没有任何节点的 arrival 超过轮初快照 ⟹ 全局 max hop 不超过轮初基线,无需回滚。pure-removal 路径天然安全(只缩小 fanin 集,arrival 只降不升),不需门槛。

### Phase 1：partition-match resub

移植自 `cmfs.cpp` 的 `try_arrival_resub`，把接受准则从"更低 arrival"换成"divisor 已经在 consumer 的分区里"——只要用同分区 divisor 替换掉跨分区的 critical fanin，这条边就直接消失，新增 0 条跨分区边。

候选收集（`collect_cut_candidates`）跟 cmfs 不同：cmfs 只扫 top-K 条关键路径，csr 扫**整个网络**里所有跨分区的 `(consumer, iFanin)` 对，因为 cutsize 是结构性的，不依赖某条路径是否关键。候选按 driver 的跨分区 fanout 总数加权排序，优先修"贡献最大的"跨分区节点。

每个候选按四级递进尝试（`try_partition_resub`）：

1. **Pure removal** — 证明该 fanin 在 ODC 下完全冗余，直接删除。
2. **Single-divisor resub** — 用一个同分区 divisor 替换。
3. **2-divisor resub** — fanin 数 ≤5 时，用两个同分区 divisor 替换（K=6 下留了一个位）。
4. **Multi-divisor + Shannon 分解**（`-X num` 门控，num=7-12）— 贪心累积 3+ 个同分区 divisor，超过 6 输入后通过 Shannon 分解拆成合法的 6-LUT 树，每个叶子/MUX 节点通过 `choose_partition_csr`（选跨分区边最少的分区）单独分配 partition。

跟 cmfs 的关键差异：cmfs 用 arrival 排序 divisor、且 Shannon 分解按最高 arrival 变量切；csr 没有 timing 信号，divisor 只按"是否同分区"二元过滤（无序），Shannon 分解固定切变量 0（分解顺序只影响树深度，cutsize 目标完全交给 `choose_partition_csr` 的按节点分区选择处理）。

每轮结束跑 `Abc_NtkSweep` + `Abc_NtkCleanup` 清理悬空节点，用 `stall_limit` 检测收敛（cut-edge 数连续 N 轮不再下降就停）。

### Phase 2：replication 兜底

Phase 1 里 resub 找不到同分区 divisor 的跨分区边（`good_divs` 为空），Phase 2 用 cpr 风格的节点复制兜底：把 driver 复制一份放到 consumer 所在分区，让 consumer 那一侧的 fanout 改接复制品。

接受准则（`try_replicate`）：

- **PI/CONST1 driver 直接拒绝**——`Abc_NtkDupObj` 会完整复制对象的 `Type`，复制 PI 会伪造出第二个 primary input，破坏网表（开发过程中踩过这个坑，见下文 bug 记录）。
- **严格局部 cut-edge 下降**：复制后重新计一次全网 cut-edge，必须比复制前更少才接受，否则回滚（撤销 `Abc_ObjPatchFanin` 改动 + 删除复制品）。
- **全局节点增长上限**（`-G num`，默认 2%）：复制的节点总数不能超过原始节点数的 `num%`。
- **Hop-slack 门槛**（见下一节）：复制后不能让该节点的 hop 变差。

一次复制会把 driver 在目标分区里的**所有**符合条件的 fanout 一起改接（不只是触发这次复制的那一条候选边），因为复制品一旦存在，同分区的其他 consumer 也该改接过去,否则复制的收益会被低估、还会留下本可以顺路修掉的边。

#### Hop-slack 门槛

Phase 2 复制会改变节点的分区归属，可能让原本不在关键 hop 路径上的节点变得跨分区、hop 数上升。为了不让"降 cutsize"反而"升 hop"，引入了跟 timing slack 完全同构的 hop-slack 概念：

```
hop_arrival(n)  = max over fanins fi of (arrival[fi] + (cross_partition(fi, n) ? 1 : 0))
max_hop         = max over all n of hop_arrival(n)
hop_required(n) = min over qualifying fanouts fo of (required[fo] - (cross_partition(n, fo) ? 1 : 0))
                  (PO 驱动节点、无 fanout 节点的 required 直接取 max_hop)
hop_slack(n)    = hop_required(n) - hop_arrival(n)
```

`slack == 0` 意味着该节点正好卡在某条最长 hop 路径上,没有增加空间;`slack == k` 意味着它的 hop 可以再涨 k 而不影响全局 max_hop。

这个 snapshot 在 Phase 2 开始前**只算一次**（不是每轮重算），是刻意的性能取舍——按候选逐个重算全网 hop 代价太高。复制发生时，只需要局部检查：复制品的新 hop_arrival（基于其 fanin 现有的 hop_arrival + 新分区下的跨分区惩罚重新算）不能超过原 driver 的 hop 预算（`hop_arrival[driver] + hop_slack[driver]`）。这是一个 O(fanins) 的局部检查，不是全网重算——因为复制只会改变复制品自己 fanin 边的跨分区状态，不会改任何其他节点的 fanin。

代价：snapshot 是静态的，不会随本轮已发生的复制动态更新，属于偏保守的方向（可能漏掉一些实际仍然合法的机会），但不会让 hop 失控。

### 收尾：可选的 balance repair（`-b`，默认关闭）

`cpr` 风格的 `enforce_balance` 只优化分区大小是否平衡，完全不看 cutsize。早期实现里默认跑这一步，实测在 `adder`/`voter`/`router`/`square`/`C880`/`usb_phy` 等 6 个 case 上把 Phase 1/2 刚降下去的 cutsize 又推高（`adder.v` 上观察到 12 → 21），是所有"负收益" case 的唯一根源。已改成默认关闭，通过 `-b` 显式开启（功能正确性不受影响，cec 验证过，只是 cutsize 目标会被牺牲）。

## 命令接口

```
usage: csr [-R num] [-S num] [-T num] [-X num] [-G num] [-N num] [-B num] [-bLv]
           cut-edge reduction via resub-first + replication-fallback logic synthesis
    -R num  : max optimization rounds per phase [default = 20]
    -S num  : stall limit (rounds without improvement) per phase [default = 3]
    -T num  : deterministic search trajectories, 1-3 [default = 1]
    -X num  : max temp LUT size for Shannon decomp (0=off, 7-12), Phase 1 only [default = 0]
    -G num  : Phase 1/2 shared non-refunding growth budget, % of entry node count [default = 2]
    -N num  : max cut-net count, % of entry cut-net count, shared Phase 1/2 budget [default = 300]
    -B num  : balance percentage (1-99) [default = inherit from pdb]
    -b      : run cpr-style balance repair after phase1/2 [default = off]
    -L      : disable phase 0 hop-preserving relocation [default = on]
    -v      : toggles verbose output
```

## 增强版架构(2026-07 更新)

CSR 现在以 `Abc_Frame_t *` 为公开入口(`ApplyCsr(Abc_Frame_t *, const Config &)`)，而不是直接接管当前网络。整个优化过程遵守以下事务模型和硬约束：

### Frame 所有权与事务模型

- `csr` 在进入时立即用 `CaptureEntryLimits` 冻结入口网络的标量状态（`num_parts`、`balance_pct`、`hop`、`cut-net`、节点数），因为 `Abc_NtkDup` 不保留 `Pdb` 的这些字段。
- 每条轨迹从入口网络的独立 `Abc_NtkDup` 快照开始；frame 里的入口网络在所有轨迹跑完之前保持不变。
- 只有胜出轨迹通过 `Abc_FrameReplaceCurrentNetwork` 安装；安装前先用 `RestorePdbMetadata` 把冻结的入口 `balance_pct`/`num_parts` 写回，恢复对后续命令可见的元数据。
- 落选轨迹和全部失败的轨迹网络都会被显式删除；全部轨迹失败时 frame 保持入口网络不变。

### 入口硬约束（`EntryLimits`）

以入口网络为基线冻结：

```text
hop_limit             = entry_hop                      (不允许任何轨迹的最终 hop 超过入口)
node_limit             = ceil(entry_nodes * 1.02)        (存活节点数上限)
growth_budget          = floor(entry_nodes * 0.02)       (Phase 1/2 共享，不返还)
cutnet_limit           = ceil(entry_cutnet * cutnet_growth_pct / 100)  (`-N`，默认 300)
balance_overflow_limit = compute_balance_overflow(entry_part_sizes, entry 分区在 balance_pct 下的 max_allowed)
```

除 `balance_overflow_limit` 外，这四个值全部来自 `docs/superpowers/specs/2026-07-11-csr-optimization-enhancement-design.md` 的原始设计（`cutnet_limit` 的初始比例定为 150%，`-N` 选项和 300% 默认值是后续调查发现的修正，见下文"cutnet 预算与 Phase 2 的相互作用"）。`balance_overflow_limit` 是实现过程中发现必须补的第五个值：hmetis 的递归二分在 `num_parts > 2` 时天然会让实际分区大小超过 `balance_pct` 声明的名义容差（误差逐层复合），而不像 hop/node/cutnet 三个约束一样"入口值天然满足自己的门槛"。若直接要求 `overflow == 0`，冻结分区语料库里 19/90 个案例会在任何优化开始之前就在入口审计失败。`balance_overflow_limit` 改为"入口态本身已有的 overflow"，审计只要求不比入口更差，与其余三个约束同构。

`node_limit` 是防御性的硬上限；正常语义下 `growth_budget` 的"不返还"规则已经保证不会触达它。

节点数增长预算 `growth_budget` 由 Phase 1（Shannon 分解）和 Phase 2（复制）共享，按累计正净增长单调消耗，删除节点不返还额度。

### `-T` 多轨迹搜索

`-T num`（1-3，默认 1）为 CSR 增加确定性的多轨迹搜索：

```text
trajectory 0 -> TrajectoryPolicy::GainFirst           (收益优先，完整稳定 tie-break)
trajectory 1 -> TrajectoryPolicy::BoundaryConcentration (边界集中优先)
trajectory 2 -> TrajectoryPolicy::ScarcityFirst        (稀缺机会优先)
```

胜出轨迹按字典序选择：cut-edge 更少 → cut-net 更少 → hop 更少 → 节点数更少 → 轨迹编号更小。所有 tie-break 使用对象 ID 和分区 ID，不依赖指针地址或哈希表遍历顺序，保证同一输入重复运行产生字节级相同的输出（已在 90 个冻结分区语料库上验证，3 次 exact-repeat 全部一致）。

### Phase 0：move / swap / compound relocation

单节点 relocation 和相邻跨分区节点 swap 收敛后，运行深度受限（种子上限 64，beam width 8，最大深度 3）的短序列 compound relocation 搜索，覆盖单步、swap 都无法表达的组合迁移（例如两步都不降 cut-edge 但整体降的序列）。中间状态必须满足 hop/cut-net/平衡硬约束；整个序列只有最终 cut-edge 严格下降才原子提交。

### Phase 1：node-level 多方案 resub + joint-replacement 快速路径

在原有逐 `(consumer, iFanin)` MFS resub 之外，新增一个 consumer 级快速路径（`RunPhase1Resub`）：检测已存在的、fanin 集合完全相同、逻辑功能相同的同类节点，用一个外分区 divisor 联合替换两个跨分区 fanin（只在新增跨分区数严格少于删除数时允许）。

这个路径必须区分网络的底层表示：`if` 映射后的网络是 `ABC_FUNC_AIG`（`pManFunc` 是 `Hop_Man_t*`，节点 `pData` 是结构哈希的 `Hop_Obj_t*`），只有旧式 SOP 网络才是 `ABC_FUNC_SOP`（`pManFunc` 是 `Mem_Flex_t*`，`pData` 是 SOP cover 字符串）。函数相同性判断和单输入 buffer 函数构造都必须按 `Abc_NtkHasAig`/`Abc_NtkHasSop` 分支处理，否则会把 AIG 网络的 `pData` 当字符串传给 `Abc_SopCreateBuf`，直接破坏 SOP 内存管理器并 segfault——这个 bug 在开发阶段一度导致 CSR 在**任何**经过 `if -K` 映射的真实电路上都必崩溃（Task 10 的单元测试只覆盖了合成的 SOP 网络，没触发这条路径）。

跨分区候选仍按 driver 的跨分区 fanout 总数加权排序；每轮结束仍跑 `Abc_NtkCleanup`（不再跑 `Abc_NtkSweep`——`Abc_NtkSweep` 会转 BDD 并通过 `Abc_ObjPatchFanin` 清除任何 <2-fanin 节点，会删掉 pdecomp 的跨分区 identity buffer 并重新打开它们本该防止的 fanout 爆炸，若 csr 跑在 pdecomp 之后会出问题）。

### Phase 2：聚合复制候选 + 复制簇 + 增量 HopState

复制候选按 `(driver_id, target_partition)` 聚合，而不是逐条 cut-edge：预先计算可消除的目标分区 fanout 数、复制品新增的跨区 fanin 数、cut-net 变化和节点成本，按净收益/节点成本降序处理。

当单独复制 driver 收益不足时，允许沿 fanin 方向构造深度 ≤2、节点数 ≤3 的复制簇（同一 driver-target 最多评估 16 个）。簇内部边全部同分区，不计入 cut-edge/cut-net；簇按原拓扑序创建，整体原子提交或回滚。

Hop 门槛从静态的入口态 hop-slack 快照，改为可增量更新的 `HopState`：Phase 2 开始时用 `Abc_NtkDfs` 建立拓扑序号并计算全部初始 arrival；每次复制/簇提交时只需局部传播受影响子图的 arrival，任一节点超过 `hop_limit` 时逆序回滚 arrival 日志和复制拓扑。`HopState::VerifyAgainstFull` 独立重算全网 arrival 向量校验增量结果，每轮结束仍全网重算 hop 作为实现审计。

## 已知问题（增强版）

- **`-b` balance repair 语义未变**：默认关闭，`-b` 显式开启后仍可能牺牲 cutsize 收益（见上文历史记录），行为经 `cec` 验证过。
- **`-T 2/3` 未纳入默认 5 倍运行时间验收范围**：设计上是显式开启的额外搜索，仍受确定的操作次数预算约束，但运行时间需单独评估。
- **vendored ABC 上游代码存在 UBSan 未定义行为诊断**：ASan/UBSan 构建下运行 `if -K` LUT 映射会在 `src/abc/src/map/if/{ifCut,ifMap,ifMan}.c` 和 `src/abc/src/sat/bsat/{satSolver,satStore}.c` 报告约 91 条"misaligned address"诊断（8 字节对齐要求上的未对齐访问）。这些文件自"Vendor ABC sources into repo"提交后未被 csr 相关改动触碰过，是上游代码的既有问题，不是 csr 引入的；`csr.cpp`/`csr_state.cpp`/`csr_hop.cpp` 本身在同一次 ASan/UBSan 运行下没有任何诊断，`test_csr` 在 ASan/UBSan 下完全无诊断退出。
- **7 个语料库案例在 200 秒超时下未跑完**：`EPFL_div`、`EPFL_hyp`、`EPFL_mem_ctrl`、`vtr_LU32PEEng`、`vtr_LU8PEEng`、`vtr_mcml`、`vtr_sha`，均为超时而非崩溃或断言失败；未逐个排查是否存在可优化的候选收集热点。
- **`-N` 不是单调参数，个别电路需要反向调整**：`opencores_wb_conmax` 在 `-N 200` 下与 pre-enhancement baseline 的差距（+402）比 `-N 300` 下（+1026）更小——多数电路"预算越松、Phase 2 收益越多"，但这个电路反过来。默认 300% 是对语料库整体最优的取值，不是对每个电路都最优；差距较大时可尝试手动调低或调高 `-N` 重跑对比。
- **仍有 14/82 个案例的 cutsize 比 pre-enhancement baseline（799702c）差**（`-N 300`，同一冻结分区）：`opencores_wb_conmax`（+1026）、`EPFL_multiplier`（+421）、`mcnc_spla`（+252）、`mcnc_pdc`（+233）、`opencores_tv80`（+220）等，未逐个排查是否为同一 cutnet 预算机制或其他原因导致，见下文"cutnet 预算与 Phase 2 的相互作用"。

### cutnet 预算与 Phase 2 的相互作用（`-N` 选项，2026-07-11 补充调查）

增强版上线后发现一个此前未暴露的问题：在同一冻结分区上，25/83 个案例的最终 cutsize 比 pre-enhancement baseline（799702c）还差，其中 `opencores_systemcase` 差距最大（+94.3%，baseline 1392 vs 增强版 2704）。

根因排查（`opencores_systemcase`，入口 cut-net=76）：`cutnet_limit` 是 Phase 1（resub）和 Phase 2（replication）**共享**的硬约束，旧实现里固定为 `ceil(entry_cutnet * 1.5)`。verbose 日志显示 Phase 1 跑完后 cut-net 已经**正好**顶到 114/114（150% 上限），Phase 2 开始尝试复制候选簇时，420 次全部被 `ClusterLimitsHold` 拒绝——包括 `cutedge_delta` 达到 -184、-137、-130 这种收益巨大的候选，拒绝原因清一色是共享预算已被 Phase 1 耗尽，不是候选本身不合法或 hop 超限。旧版基线没有这个约束，Phase 2 直接把 cut-net 推到 220（entry 的 290%），换来 cut-edge 2654→1392 的大幅收益。

修复：把 150% 这个比例从硬编码改成可调的 `-N` 选项（`cfg.cutnet_growth_pct`，`CaptureEntryLimits`/`csr_state.cpp` 消费）。用 `systemcase` 测试不同取值：150%/200% 下 Phase 2 replicated 数始终是 0，直到把限制放宽到接近不设上限，Phase 2 才能正常工作（最终 cut-net 落在 entry 的 296%）。在全量语料库上扫了 150%/200%/300% 三档，300% 是让**总量**从"比 baseline 差 4.4%"翻到"比 baseline 好 1.2%"的取值，遂定为新默认值。这不是一次性调对——`wb_conmax` 在 300% 下反而比 200% 更差（见上文已知问题），说明固定比例这条路本身有结构性局限：不同电路需要的 cutnet 宽裕度方向不一致，真正根治需要把 Phase 1/Phase 2 的预算分开计算，而不是共享一个入口相对值；这次调查止于把默认值调到语料库整体最优，未做预算隔离。

## 实测结果（增强版，2026-07-11）

在 90 个 SimpleCircuits benchmark（MCNC + EPFL + OpenCores + VTR）上，N=4 hmetis 分区（**冻结分区**，`--save-part`/`--load-part` 保证可复现），`csr -T 1 -v`，对比**入口态**（不跑 csr 时的 cut-edge），3 次 exact-repeat 验证确定性，`--cec` 验证功能等价：

| 指标 | 数值 |
|---|---|
| 跑通且有 cut-edge 收益的案例 | 84/85（唯一零收益案例 `vtr_bgm` 的入口 cut-edge 本身就是 0） |
| 负增益案例 | **0** |
| 有收益案例总 cut-edge | 195887 → 136675（**-30.2%**） |
| 总节点数（跑通案例） | 379293 → 380940（+0.43%，61/85 案例变胖，在 2% 增长预算内） |
| 总 hop（跑通案例） | 328 → 317（**-3.4%，净改善**） |
| 硬约束违规（hop/节点数/cut-net/cut-edge） | **0/85** |
| 3 次 exact-repeat 确定性 | **85/85 完全一致** |
| cec 功能等价性 | **85/85 EQ，0 NOT_EQ** |
| 默认 `-T 1` 运行时几何均值（vs baseline） | **1.99x**（5 倍验收线内） |
| 运行时超 5 倍的案例 | `EPFL_voter`、`opencores_DMA`、`EPFL_sqrt`、`vtr_boundtop`、`opencores_fpu`、`vtr_bgm`、`vtr_LU8PEEng`（均为增强版多阶段搜索换取更大收益的代价，未违反任何硬约束） |
| 未跑完的案例（200 秒超时） | 5 个：见上文"已知问题" |

ASan/UBSan：`test_csr` 在 asan 构建下完全无诊断输出退出；`csr.cpp`/`csr_state.cpp`/`csr_hop.cpp` 在实际 `if -K 6` + `csr -T 1` 运行下也没有任何诊断（全部诊断落在 vendored ABC 上游代码，见上文"已知问题"）。

### 与 pre-enhancement baseline（799702c）的对比

上一节的数字是"增强版 vs 不跑 csr 的入口态"，衡量的是 csr 本身有没有用。这一节换一个基准：**同一份冻结分区，增强版 vs 增强前 commit 799702c**，衡量的是这次 Tasks 1-11 的重构本身让 csr 变好了还是变差了。两者都是有效指标，但回答的是不同问题，不能互相替代。

在 `-N` 默认值调整过程中（见上文"cutnet 预算与 Phase 2 的相互作用"），这个对比先后在三档 `-N` 取值下测过：

| `-N` 取值 | 总 cut-edge 新版 vs 老版 | 相对变化 | 变好/变差/持平 | 运行时几何均值 |
|---|---|---|---|---|
| 150%（未加此选项前的固定值） | 132026 vs 126507 | **+4.4%（更差）** | 48/25/10 | 1.99x |
| 200% | 129253 vs 128824 | +0.3% | 53/21/10 | 未单独测 |
| **300%（当前默认）** | 118889 vs 120330 | **-1.2%（更好）** | **58/14/10** | **1.64x** |

`-N 300` 下的完整数据（84/90 案例跑通并有 baseline 数据，`csr -T 1 -v`）：

| 指标 | 数值 |
|---|---|
| 参与对比的案例 | 82（另 7 个案例双方均超 200 秒超时或缺基线数据，1 个 `vtr_bgm` 入口即为 0） |
| 比老版更好 / 更差 / 持平 | 58 / 14 / 10 |
| 总 cut-edge（新版 vs 老版） | 118889 vs 120330（**-1.2%**） |
| 运行时几何均值（新版 vs 老版） | **1.64x** |
| 仍比老版差的案例（前 5） | `opencores_wb_conmax`（+1026）、`EPFL_multiplier`（+421）、`mcnc_spla`（+252）、`mcnc_pdc`（+233）、`opencores_tv80`（+220） |

结论：把 `cutnet_limit` 比例从硬编码 150% 提到可调的 `-N`，默认值定为 300%，是让增强版在"同分区对比老版"这个指标上由负转正的关键调整；单看"vs 入口态"的 -30.2% 收益数字不受这次调整影响（该对比不涉及老版基线），但"vs 老版基线"这个更严格的指标在改之前是隐藏的负收益。

以上数字全部来自 `scripts/run_csr_regression.py --exact-repeats 3 --cec --baseline-foxsyn <799702c 二进制>` 在冻结分区语料库上的实际测得结果，不是历史遗留估算。

典型用法：

```
read design.v; st; if -K 6; hpart -N 4; csr -v
ps
```

## 文件结构

```
src/csr/
  CMakeLists.txt   — 构建配置，链接 libabc + timer + cpr
  csr.hpp          — Config 结构体 + ApplyCsr / ComputeCutEdgeCount 声明
  csr.cpp          — 核心实现（~890 行）
```

`cpr.hpp`/`cpr.cpp` 里的 `partition_sizes` / `compute_balance_max_allowed` / `compute_balance_overflow` / `enforce_balance` 四个函数从 `static` 改成了公开 API，供 csr 直接链接调用，避免代码复制。

## 开发过程中的问题

### Bug 1：缺 reverse levels 初始化，断言崩溃

Phase 1 的 MFS 窗口构建循环最初没有调用 `Abc_NtkStartReverseLevels`/`Abc_NtkStopReverseLevels`，触发 `Abc_ObjRequiredLevel: Assertion 'pNtk->vLevelsR' failed`（`abcTiming.c:1217`）。修复：在 `Mfs_Par_t`/`Mfs_ManAlloc` 之前加 `Abc_NtkLevel(pNtk); Abc_NtkStartReverseLevels(pNtk, 0);`，`Mfs_ManStop` 之后加 `Abc_NtkStopReverseLevels(pNtk);`。

### Bug 2：Phase 2 复制 PI 节点，伪造多余 primary input

`try_replicate` 最初对任意 driver 无差别调用 `duplicate_node_csr`（内部是 `Abc_NtkDupObj`）。`Abc_NtkDupObj` 会完整复制对象的 `Type`——复制一个 PI 就是真的多出一个 primary input。表现为 `i/o` 计数异常（多出的输入数正好等于该轮复制次数），写 BLIF 时报 `NetworkCheck: CI with ID ... is in the network but not in the name table`。根因是没有像 cpr 已有的 `ordered_path_nodes` 那样限制只复制 `Abc_ObjIsNode` 的对象。修复：`try_replicate` 开头加 `if (!Abc_ObjIsNode(pDriver)) return false;`。

### Bug 3：Phase 2 不看 hop，把 timing 推坏

最初 Phase 2 唯一的接受准则是"cut-edge 严格下降"，完全不管 hop。在 `div`/`sqrt` 等 benchmark 上观察到 hop 从个位数被推到 46-65（一轮内多次复制级联放大）。通过 `-G 0`（关闭 Phase 2）做对照实验，确认 Phase 1 的 resub 从不改变 hop（因为只换 divisor，不改分区归属），问题完全在 Phase 2。修复：引入上面的 hop-slack 门槛机制。

### Bug 4：balance repair 默认开启，吃掉 cutsize 收益

见上文"收尾"小节。通过对比"跑 balance repair 前/后的 cutsize"发现所有负收益 case 都是这一步造成的。修复：改成 `-b` 显式开启，默认关闭。

## 实验结果

在 90 个 SimpleCircuits benchmark（MCNC + EPFL + OpenCores + VTR）上，N=4 分区，`--cec` 验证功能等价性：

### 修复 hop-slack 门槛 + balance repair 默认关闭之前

| 指标 | 数值 |
|---|---|
| 有 cut-edge 收益 | 69/90 |
| 负增益 case | 6（adder/voter/router/square/C880/usb_phy，全部因 balance repair） |
| hop 数变化 | +21.5%（Phase 2 级联复制推高） |
| cec | 79/79 EQ |

### 修复之后（当前状态）

| 指标 | 数值 |
|---|---|
| 有 cut-edge 收益 | 70/90 |
| 负增益 case | **0** |
| 总 cut-edge（有收益 case） | 163802 → 123720（**-24.5%**） |
| 总节点数（跑通 case） | 311590 → 312649（+0.3%，54/79 变胖） |
| 总 hop（跑通 case） | 335 → 315（**-6%，净改善**） |
| cec | 79/79 EQ，0 NOT_EQ |
| crash/fail | 11 个（`frg2`/`x4` 等，见下） |

`-b` 手动开启后在 `adder.v` 上复现了预期的 cutsize 回退（11 → 28），确认该开关行为符合设计。

### LUT size（`-K`）敏感性

多die FPGA 论文 ResynMD（Interconnect-Aware Logic Resynthesis for Multi-Die FPGAs，2026）报告过一个规律：LUT 越小，resub 消除跨分区边的效果越好（LUT4 > LUT5 > LUT6，单调递减），原因是小 LUT 把逻辑拆得更细，节点数更多、每个节点的同分区候选 divisor 池子更大。该论文为了看清这个单一变量，把分区器换成了确定性 hash 分区，规避了 hMetis 划分质量本身随网表结构波动这个混杂因素。

在 `csr` 上用同款 EPFL 11 个 case（`hyp` 超时排除）、`hpart -N 4`（仍走 hMetis，未做上述隔离）测了 `-K 2/3/4/5/6` 五档。首次单次运行发现结果不单调、跟论文对不上，进一步排查发现 **hMetis 划分本身带运行间随机性**（同一条命令重跑，多数 case 波动 1-2 个百分点，少数 case 到 4%+，个别 case 如 `max`/`log2` 在特定 K 下呈双峰分布，标准差可达 10-34 个百分点）——单次运行的对比结论不可信。改为每个配置跑 5 次取均值：

| K | 平均削减率(5 次均值) |
|---|---|
| K2 | 32.97% |
| K3 | 27.41% |
| K4 | 25.23% |
| K5 | 25.16% |
| K6 | 27.98% |

均值化后结论不变：只有 K2 这种极端小 LUT 才有明显抬升，**K3 到 K5 基本走平（27.41% → 25.16%），K6 又回升到 27.98%**，跟论文报告的清晰单调趋势不同，且不单调。逐 case 噪声量级差异很大——多数 case 标准差在 0.3-3 个百分点内，但 `K2 max`（std=34.12，原始 5 次值在 0.7% 和 63.0% 间跳变，双峰分布）、`K2 log2`（std=12.01）、`K6 max`（std=9.91）、`K5 bar`（std=6.81）这类 case 的单次测量完全不可信，是之前单次运行结论跑偏的主因。

结论：`csr` 的收益率主要由电路结构决定，K 值是次要因素，且这个效应在常用范围（K4-K6）里基本可忽略。之所以没能重现论文那种干净的单调趋势，一部分是因为这组实验每个 K 都重新走了一遍 hMetis——划分质量本身随网表结构（不同 K 产出的节点数、连接结构都不同）的波动、加上 hMetis 运行间的随机性，共同盖过了"LUT size 影响 divisor 池子"这个真正想测的效应。若要干净复现论文的对比方法，需要像论文一样换成确定性 hash 分区，本次未做（见"后续方向"）。

### rewrite 预处理（AIG 层面重写）对 K2 的影响

顺带测了一个假设：K2 几乎等同门级网表，若先在 AIG 上跑技术无关的 `rewrite`（局部子图替换，收紧 AND 门数量/深度）再映射到 K2，是否能进一步提升 `csr` 收益。同样每配置 5 次均值，`rewrite; rewrite -z; if -K 2` vs 直接 `if -K 2`：

| 配置 | 平均削减率(5 次均值) |
|---|---|
| K2 | 32.97% |
| K2 + rewrite | 34.93% |

平均小幅正收益（+1.96 个百分点），但**逐 case 方向不一致，个别电路有明显负效应**：`log2`(+14.95%)、`max`(+25.02%) 明显受益，但 `square`(**-16.74%**)、`multiplier`(-5.10%)、`voter`(-4.61%)、`mem_ctrl`(-3.17%) 反而变差。`square` 的负效应相对其自身噪声（两组 std 分别 0.41/2.72）看起来是真实效应，不是采样误差。推测 `rewrite` 改变了 AIG 结构布局，进而改变映射到 K2 后 hMetis 的分区结果——这个改变对某些电路创造出更多同分区 divisor 机会，对另一些则打散了原本存在、`csr` phase1 能利用的局部逻辑共享结构，是电路结构相关的双向效应，不是普适收益。

## 已知问题

- **11 个 benchmark crash/fail/timeout**：`frg2`、`x4` 崩溃已确认是 vendored ABC 上游 `abcFunc.c` 的通用 bug（`Abc_NtkBddToSop` 常量折叠节点时留下悬空 `vFanouts` 引用），**已在 commit `3b70cbd` 修复**，与 csr 本身无关，见 `docs/cmfs.md` 已知问题一节。剩余 `LU8PEEng`、`mkDelayWorker32B`、`blob_merge`、`mkSMAdapter4B`、`LU32PEEng`、`stereovision1`（此项已随上面的修复解决）、`hyp`、`mcml`、`sha` 多为大设计在 120s 回归超时下的超时，未逐个排查是否为真崩溃。
- **hop-slack snapshot 是静态的**：只在 Phase 2 开始前算一次，不随本轮已发生的复制动态更新。偏保守，可能漏掉部分本来合法的复制机会，但不会让 hop 失控。
- **`hpart`（hMetis）划分带运行间随机性**：同一条 `read; ...; hpart -N num` 命令重复执行，`cut size`/`hop num` 会在小范围内波动（多数电路 1-2%，少数到 4%+，个别电路+参数组合下甚至呈双峰分布，标准差可达两位数百分点，见"LUT size 敏感性"一节的排查记录）。`hpart` 未暴露种子控制 flag，只有 `--save-part`/`--load-part` 能冻结分区结果供复现实验用。测量 `csr` 收益率时，若不使用冻结分区，应多次采样取均值而非信任单次运行。
- 候选收集是全网扫描（不像 cmfs 限定 top-K 关键路径），大网络上 Phase 1 每轮的候选收集本身有 O(nodes) 开销。

## 后续方向

- 与 `cpr`/`cmfs` 交替迭代：csr 降 cutsize → cpr 用降下来的 cutsize 空间做 hop 优化 → csr 再找新机会。
- Phase 2 的 hop-slack snapshot 改成增量更新（每次复制后局部刷新受影响节点的 slack），换取更激进但仍然安全的复制策略。
- 排查剩余 timeout case 是否有可优化的候选收集 / SAT 求解热点。
- 用确定性 hash 分区重跑 LUT size（`-K`）敏感性实验，隔离 hMetis 划分质量波动这个混杂变量，干净复现/反驳 ResynMD 论文报告的单调趋势。hMetis 本身还带运行间随机性（"已知问题"），换分区器之外仍需多次采样取均值。
- 排查 `rewrite` 对 K2 收益的双向效应（`square`/`multiplier` 变差，`log2`/`max` 变好）具体是哪类结构变化触发的，评估是否值得在 `csr` 前置一个"先 rewrite 再评估是否映射到更小 K"的启发式判断。

## 相关文件

- `src/main.cpp` — 命令行解析与注册（`Csr_Command`）
- `src/csr/csr.hpp` / `src/csr/csr.cpp` — 核心实现
- `src/cpr/cpr.hpp` / `src/cpr/cpr.cpp` — `enforce_balance` 等平衡修复原语（被 csr 复用）
- `scripts/run_csr_regression.py` — 回归脚本（cutsize/area/hop 对比 + `--cec` 功能等价性）
- `docs/cmfs.md` — 关键路径 resub 的姊妹命令，含 frg2/x4 崩溃根因记录
- `docs/hpart.md` — 分区数据来源
