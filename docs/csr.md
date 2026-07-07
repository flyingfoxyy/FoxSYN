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
usage: csr [-R num] [-S num] [-X num] [-G num] [-B num] [-bLv]
           cut-edge reduction via resub-first + replication-fallback logic synthesis
    -R num  : max optimization rounds per phase [default = 20]
    -S num  : stall limit (rounds without improvement) per phase [default = 3]
    -X num  : max temp LUT size for Shannon decomp (0=off, 7-12), Phase 1 only [default = 0]
    -G num  : Phase 2 replication node growth cap, % of original node count [default = 2]
    -B num  : balance percentage (1-99) [default = inherit from pdb]
    -b      : run cpr-style balance repair after phase1/2 [default = off]
    -L      : disable phase 0 hop-preserving relocation [default = on]
    -v      : toggles verbose output
```

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
