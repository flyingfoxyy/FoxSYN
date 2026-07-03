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

    Phase 1: run_phase1_resub      -- 优先尝试不增加节点的 SAT resub
    after_phase1 = ComputeCutEdgeCount(pNtk)

    Phase 2: run_phase2_replicate  -- resub 打不动的边，用节点复制兜底
    after_phase2 = ComputeCutEdgeCount(pNtk)

    if cfg.do_balance_repair (-b):
        cpr::enforce_balance(...)  -- 可选的收尾分区平衡修复，默认关闭

    final_cutedges = ComputeCutEdgeCount(pNtk)
```

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
usage: csr [-R num] [-S num] [-X num] [-G num] [-B num] [-bv]
           cut-edge reduction via resub-first + replication-fallback logic synthesis
    -R num  : max optimization rounds per phase [default = 20]
    -S num  : stall limit (rounds without improvement) per phase [default = 3]
    -X num  : max temp LUT size for Shannon decomp (0=off, 7-12), Phase 1 only [default = 0]
    -G num  : Phase 2 replication node growth cap, % of original node count [default = 2]
    -B num  : balance percentage (1-99) [default = inherit from pdb]
    -b      : run cpr-style balance repair after phase1/2 [default = off]
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

## 已知问题

- **11 个 benchmark crash/fail/timeout**：`frg2`、`x4` 崩溃已确认是 vendored ABC 上游 `abcFunc.c` 的通用 bug（`Abc_NtkBddToSop` 常量折叠节点时留下悬空 `vFanouts` 引用），**已在 commit `3b70cbd` 修复**，与 csr 本身无关，见 `docs/cmfs.md` 已知问题一节。剩余 `LU8PEEng`、`mkDelayWorker32B`、`blob_merge`、`mkSMAdapter4B`、`LU32PEEng`、`stereovision1`（此项已随上面的修复解决）、`hyp`、`mcml`、`sha` 多为大设计在 120s 回归超时下的超时，未逐个排查是否为真崩溃。
- **hop-slack snapshot 是静态的**：只在 Phase 2 开始前算一次，不随本轮已发生的复制动态更新。偏保守，可能漏掉部分本来合法的复制机会，但不会让 hop 失控。
- 候选收集是全网扫描（不像 cmfs 限定 top-K 关键路径），大网络上 Phase 1 每轮的候选收集本身有 O(nodes) 开销。

## 后续方向

- 与 `cpr`/`cmfs` 交替迭代：csr 降 cutsize → cpr 用降下来的 cutsize 空间做 hop 优化 → csr 再找新机会。
- Phase 2 的 hop-slack snapshot 改成增量更新（每次复制后局部刷新受影响节点的 slack），换取更激进但仍然安全的复制策略。
- 排查剩余 timeout case 是否有可优化的候选收集 / SAT 求解热点。

## 相关文件

- `src/main.cpp` — 命令行解析与注册（`Csr_Command`）
- `src/csr/csr.hpp` / `src/csr/csr.cpp` — 核心实现
- `src/cpr/cpr.hpp` / `src/cpr/cpr.cpp` — `enforce_balance` 等平衡修复原语（被 csr 复用）
- `scripts/run_csr_regression.py` — 回归脚本（cutsize/area/hop 对比 + `--cec` 功能等价性）
- `docs/cmfs.md` — 关键路径 resub 的姊妹命令，含 frg2/x4 崩溃根因记录
- `docs/hpart.md` — 分区数据来源
