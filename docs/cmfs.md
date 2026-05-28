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

窗口的 TFO 深度（`-W` 参数）决定了 ODC 的精度：更深的窗口提供更多 don't-care 信息。

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

### 迭代结构

```
for each round:
    SOP→AIG 转换（确保 Hop manager 一致性 + MinimumBase 移除局部冗余）
    计算 levels 和 reverse levels
    compute_arrival → extract_top_paths(K)
    收集 critical fanin 候选，计算权重，降序排列
    初始化 MFS manager
    对每个候选依次尝试：
      1. Pure removal
      2. Single-divisor resub（-r 模式）
      3. 2-divisor resub（-r 模式，fanin ≤ 5）
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
usage: cmfs [-K num] [-R num] [-S num] [-C num] [-W num] [-F num] [-M num] [-rv]
    -K num  : 分析的关键路径数量 [default = 16]
    -R num  : 最大优化轮数 [default = 20]
    -S num  : 停滞轮数限制 [default = 3]
    -C num  : 每次 SAT 冲突上限 [default = 5000]
    -W num  : MFS 窗口 TFO 层数 [default = 2]
    -F num  : MFS 窗口最大扇出数 [default = 30]
    -M num  : MFS 窗口最大节点数 [default = 300]
    -r      : 启用 arrival-aware resubstitution（含 2-divisor）
    -v      : 详细输出
```

### 典型使用流程

```
read design.v; st; if -K 6; hpart -N 16; cmfs -r -v -K 64 -W 6 -M 1000
```

### 实验结果

在 90 个 benchmark（MCNC + EPFL + OpenCores + VTR）上测试，16 分区，`-r -K 64 -W 6 -M 1000`：

| Case | Arrival Before | Arrival After | Gain | Hop Change |
|------|---------------|--------------|------|-----------|
| alu4 | 2215 | 2014 | 201 | 11→10 |
| tv80 | 1812 | 1412 | 400 | 9→7 |
| router | 1811 | 1611 | 200 | 9→8 |
| i10 | 2212 | 2012 | 200 | 11→10 |
| spla | 1609 | 1409 | 200 | 8→7 |
| stereovision3 | 1005 | 805 | 200 | 5→4 |
| sin | 4438 | 4240 | 198 | 22→21 |
| div | 27408 | 27210 | 198 | 133→132 |
| or1200 | 1815 | 1626 | 189 | 9→8 |

**9/90 个 benchmark 有 timing gain**，最大改善 400（2 hops），多数为 200（1 hop）。

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
2. **更大窗口**：增大 `-W` 提供更多 ODC 信息，但需平衡 SAT 求解时间。
3. **Pre-mapping 应用**：在 AIG 阶段逻辑冗余更多，但需要不同的 timing 模型。

