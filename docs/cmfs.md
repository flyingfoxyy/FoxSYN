# cmfs — Critical-Path MFS Edge Removal & Resubstitution

## 背景

FoxSYN 的 FPGA 综合流程中，网表经过 technology mapping（`if -K 6`）后，通过 `hpart` 将网络划分为多个分区，再用 `cpr` 优化跨分区延迟。在这个模型中，timing 由 hop 数主导：每条跨分区边贡献 HOP_DLY=200 的延迟，而分区内部边的延迟为 0（LUT 延迟为 1）。

`cpr` 通过两种手段减少 hop：
- **Relocate**：将关键路径上的节点段搬到相邻分区
- **Replicate**：复制跨分区节点到目标分区

但 `cpr` 不能改变逻辑功能——它只调整分区归属。`cmfs` 利用 ABC 的 MFS（Minimization with complete don't-cares）模块中的 SAT-based ODC 冗余检测，通过三种递进策略优化关键路径：

1. **Pure removal**：证明 critical fanin 在 ODC 下冗余，直接删除
2. **Single-divisor resub**：用一个 arrival 更低的 divisor 替换 critical fanin
3. **2-divisor resub**：用两个 arrival 更低的 divisor 替换 critical fanin

## 核心思路

### 候选边的选取

对于 top-K 条关键路径上的每个逻辑节点 X，计算其每个 fanin 的 arrival 贡献：

```
contribution(Fi) = arrival[Fi] + edge_delay(Fi, X)
```

其中 `edge_delay` 在跨分区时为 200，同分区时为 0。贡献最大的 fanin 即为 critical fanin。

关键洞察：**任何 critical fanin edge 都是有效目标**——不仅限于 cut-net 边。移除或替换一条 critical fanin 能打断路径、降低 arrival。

### 权重函数

```
weight(edge) = fanin_slack × frequency
```

- `fanin_slack`：critical fanin 贡献与次优 fanin 贡献之差。slack 越大，移除后 arrival 下降越多。
- `frequency`：该 edge 出现在 top-K 路径中的次数。频次越高，移除后受益的路径越多。

### SAT-based ODC 检测

MFS 窗口构建以 pNode 为中心的局部电路，通过 TFO cone 的 roots 定义 observability 边界。SAT 查询的语义是："修改 pNode 的函数后，窗口输出是否会变化？"

- **UNSAT** → 修改不可观测 → fanin 在 ODC 集合内 → 可以安全移除/替换
- **SAT** → 存在输入使输出变化 → fanin 功能上必需

窗口的 TFO 深度（`-W` 参数）决定了 ODC 的精度：更深的窗口提供更多 don't-care 信息，同时也暴露更多 divisor。

### Iterative Deepening Window（-D 模式）

固定 `-W` 深度对不同节点并非最优：浅窗口求解快但可能找不到机会，深窗口机会多但每次求解慢、且对简单节点是浪费。`-D num` 启用迭代加深——对每个候选节点，从 `nWinTfoLevs`（`-W` 值）起逐层加深窗口直到 `num`，一旦某个深度 resub 成功就停止：

```
for depth = W, W+1, ..., D:
    Abc_WinNode(p, pNode) with nWinTfoLevs = depth
    尝试 removal / resub
    成功 → break
求解后恢复 nWinTfoLevs = W
```

`-D 0`（默认）关闭迭代加深，保持原有单一深度行为。`-D` 必须大于 `-W` 才生效。

**收益来源**：实测中迭代加深新增覆盖的 case（如 ex5p）走的是 **resub 替换路径**，而非纯删除。更深的窗口之所以有效，主要是 `Abc_MfsComputeDivisors` 在更大的 window 里找到了更多候选 divisor，让 arrival-aware 替换有了可用对象——而不是 ODC 集合本身变大。这一点决定了后续的加速优化方向（见「后续方向」）。


### Arrival-Aware Resubstitution（-r 模式）

当 pure removal 失败时，尝试用 arrival 贡献更低的 divisor 替换 critical fanin：

```
acceptance criterion: contribution(divisor) < contribution(critical_fanin)
```

即使 divisor 在不同分区，只要其 `arrival + hop_delay` 比当前 critical fanin 低，替换后该节点的 arrival 就会下降。

Divisor 按 arrival 贡献升序排列，优先尝试最低 arrival 的（最大 gain）。

### 2-Divisor Resubstitution

当单 divisor 替换也失败时，对 fanin 数 ≤5 的节点（K=6 下有空间加一个 fanin），尝试用两个低 arrival divisor 替换：

```
移除 1 fanin + 加入 2 divisors → 节点从 N fanin 变为 N+1（需 N+1 ≤ 6）
```

很多函数不能用 1 个新输入表达，但可以用 2 个。搜索使用 counter-example 驱动的 OR 兼容性检查：

```
compatible_pair(d1, d2): sim[d1] | sim[d2] == all-ones (for all counter-example words)
```

这与 ABC 的 `Abc_NtkMfsSolveSatResub2` 使用相同的模式。

### Multi-Divisor Resub with Shannon Decomposition（-X 模式）

当 1-2 divisor 替换都失败时，如果指定了 `-X num`（num=7-12），允许找 3+ 个低 arrival divisor，临时超过 6 输入，然后通过 Shannon 分解将过大节点拆分为合法的 6-LUT 树。

```
Phase 4 (guarded by -X num > 0):
  greedy 累积 divisor，每加一个就尝试 SAT
  成功后：
    Abc_NtkMfsInterplate → 获得 Hop 函数
    Hop_ManConvertAigToTruth → 转为 truth table
    shannon_decompose → 递归拆分为 6-LUT 树
    替换原节点
```

Shannon 分解策略：**在最高 arrival 变量上分解**（该变量作为 MUX select，只过 1 层 LUT）。低 arrival 变量进入 cofactor（多过 1-2 层 LUT，但它们有 slack 可以承受）。

Tradeoff：`HOP_DLY=200` vs `LUT_DLY=1`。即使加 4 层 LUT（cost=4）来省 1 个 hop（gain=200），净收益 196。

### 迭代结构

```
for each round:
    SOP→AIG 转换（确保 Hop manager 一致性 + MinimumBase 移除局部冗余）
    计算 levels 和 reverse levels
    compute_arrival → extract_top_paths(K)
    收集 critical fanin 候选，计算权重，降序排列
    初始化 MFS manager
    对每个候选依次尝试（若启用 `-D`，下面整组尝试会在 W..D 各深度上迭代加深，命中即停）：
      1. Pure removal
      2. Single-divisor resub（-r 模式）
      3. 2-divisor resub（-r 模式，fanin ≤ 5）
      4. Multi-divisor + Shannon decomp（-X 模式，fanin ≤ maxTempLut）
    释放 MFS manager
    Abc_NtkSweep + Abc_NtkCleanup（清理悬空节点）
    检查收敛（stall detection）
```

## 实现情况

### 文件结构

```
src/cmfs/
  CMakeLists.txt   — 构建配置，链接 libabc + timer
  cmfs.hpp         — Config 结构体 + ApplyCmfs 声明
  cmfs.cpp         — 核心实现（~220 行）
```

### 命令接口

```
usage: cmfs [-K num] [-R num] [-S num] [-C num] [-W num] [-F num] [-M num] [-X num] [-D num] [-rv]
    -K num  : 分析的关键路径数量 [default = 16]
    -R num  : 最大优化轮数 [default = 20]
    -S num  : 停滞轮数限制 [default = 3]
    -C num  : 每次 SAT 冲突上限 [default = 5000]
    -W num  : MFS 窗口 TFO 层数（迭代加深的起始深度）[default = 2]
    -F num  : MFS 窗口最大扇出数 [default = 30]
    -M num  : MFS 窗口最大节点数 [default = 300]
    -X num  : Shannon 分解最大临时 LUT 大小 (0=off, 7-12) [default = 0]
    -D num  : 迭代加深最大 TFO 深度 (0=off，需 > W 才生效) [default = 0]
    -r      : 启用 arrival-aware resubstitution（含 2-divisor 和 Shannon decomp）
    -v      : 详细输出
```

### 典型使用流程

```
read design.v; st; if -K 6; hpart -N 16; cmfs -r -X 8 -v -K 64 -W 2 -D 6 -M 1000
```

### 复现实验（固定分区）

hmetis 分区有随机性，导致不同运行结果不可比。为做公平对比（如 `-D` 开关前后），`hpart` 支持保存/复用分区：

```
# 第一遍：跑 hmetis 并保存每个 case 的分区
hpart -N 16 --save-part design.part

# 后续：复用固定分区，跳过 hmetis
hpart -N 16 --load-part design.part
```

回归脚本对应支持 `--save-parts-dir` 和 `--load-parts-dir`：

```
python3 scripts/run_cmfs_regression.py -N 16 --cmfs-args "..." --save-parts-dir parts_N16
python3 scripts/run_cmfs_regression.py -N 16 --cmfs-args "... -D 6" --load-parts-dir parts_N16
```

### 实验结果

在 90 个 benchmark（MCNC + EPFL + OpenCores + VTR）上测试，16 分区，`-r -X 8 -K 64 -W 6 -M 1000`，每个 case 跑 3 次取最优（消除 hmetis 随机性）：

| Case | Arrival Before | Arrival After | Gain | Hop Change |
|------|---------------|--------------|------|-----------|
| tv80 | 1812 | 1210 | 602 | 9→6 |
| or1200 | 1815 | 1613 | 202 | 9→8 |
| alu4 | 2215 | 2014 | 201 | 11→10 |
| router | 1811 | 1611 | 200 | 9→8 |
| apex2 | 1006 | 806 | 200 | 5→4 |
| ex5p | 1408 | 1208 | 200 | 7→6 |
| mem_ctrl | 1812 | 1612 | 200 | 9→8 |
| stereovision3 | 1005 | 805 | 200 | 5→4 |
| div | 27408 | 27210 | 198 | 133→132 |
| fpu | 6169 | 5971 | 198 | 30→29 |
| sin | 4438 | 4435 | 3 | 22→22 |

**11/90 个 benchmark 有 timing gain**，最大改善 602（3 hops），多数为 ~200（1 hop）。

### Gain 的来源分析

Gain 来自两个机制的叠加：

1. **Abc_NtkMinimumBase**（SOP 转换副作用）：移除所有局部冗余 fanin。这是一个全局 pass，捕获 mapper 遗留的未使用输入。
2. **SAT-based arrival-aware resub**：利用 ODC 证明 critical fanin 可被低 arrival divisor 替换。这捕获了局部不冗余但全局（在 observability context 下）可替换的情况。

### 已知问题

- 少数 benchmark（frg2, x4, stereovision1 等）仍有 crash，与特定网络结构相关。
- 每轮需要 SOP→AIG 重新转换以保证 Hop manager 一致性，有一定开销。
- hmetis 分区的随机性导致每次运行结果略有波动。

### 后续方向

1. **与 cpr 交替迭代**：cmfs 移除 edge → cpr relocate → cmfs 再找新机会。
2. **更大窗口 / 迭代加深**：`-D` 已实现迭代加深。固定分区对比（`-D 0` vs `-D 6`，复用同一 hmetis 分区）显示加深仅新增 1 个 case（ex5p，gain=200），其余持平。收益真实但有限，代价是每个节点要重建多次 window+SAT。
3. **仿真 pre-filter 加速（暂缓）**：原计划用随机仿真在调 SAT 前预筛，跳过注定失败的求解，以摊薄迭代加深的开销。分析后暂缓，原因如下：
   - **节点级仿真（按 critical fanin 是否影响 window roots 判断）只对纯删除路径 sound**。而迭代加深实测新增的 ex5p 走的是 **resub 替换路径**——即使 fanin「有影响」，仍可能用 divisor 替换成功，所以该判据会漏掉真正可优化的 case。
   - 迭代加深的收益本质来自**更深 window 暴露更多 divisor**，而非 ODC 集合变大。因此真正对症的加速是 **divisor 级 signature 过滤**（预算每个 divisor 的仿真签名，按功能匹配度引导 resub），而非节点级 ODC 过滤。
   - 结论：若后续要加速，应做 divisor signature 过滤，而不是节点级仿真 pre-filter。当前先保留迭代加深的朴素实现，待确认收益规模值得优化后再投入。
4. **Pre-mapping 应用**：在 AIG 阶段逻辑冗余更多，但需要不同的 timing 模型。

