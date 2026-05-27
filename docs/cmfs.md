# cmfs — Critical-Path MFS Edge Removal

## 背景

FoxSYN 的 FPGA 综合流程中，网表经过 technology mapping（`if -K 6`）后，通过 `hpart` 将网络划分为多个分区，再用 `cpr` 优化跨分区延迟。在这个模型中，timing 由 hop 数主导：每条跨分区边贡献 HOP_DLY=200 的延迟，而分区内部边的延迟为 0。

`cpr` 通过两种手段减少 hop：
- **Relocate**：将关键路径上的节点段搬到相邻分区
- **Replicate**：复制跨分区节点到目标分区

但 `cpr` 不能改变逻辑功能——它只调整分区归属。如果一条关键路径上的某个节点，其 critical fanin 在逻辑上是冗余的（即在 don't-care 集合下该节点的功能不依赖这个 fanin），那么直接移除这条边就能打断关键路径，使该节点的 arrival 由次优 fanin 决定，从而降低整条路径的延迟。

`cmfs` 正是利用 ABC 的 MFS（Minimization with complete don't-cares）模块中的 SAT-based 冗余检测来实现这一点。

## 核心思路

### 候选边的选取

对于 top-K 条关键路径上的每个逻辑节点 X，计算其每个 fanin 的 arrival 贡献：

```
contribution(Fi) = arrival[Fi] + edge_delay(Fi, X)
```

其中 `edge_delay` 在跨分区时为 200，同分区时为 0。贡献最大的 fanin 即为 critical fanin。

关键洞察：**任何 critical fanin edge 都是有效目标**——不仅限于 cut-net 边。移除一条分区内部的 critical fanin 同样能打断路径、降低 arrival。

### 权重函数

```
weight(edge) = fanin_slack × frequency
```

- `fanin_slack`：critical fanin 贡献与次优 fanin 贡献之差。slack 越大，移除后 arrival 下降越多。
- `frequency`：该 edge 出现在 top-K 路径中的次数。频次越高，移除后受益的路径越多。

Slack 为 0 的边（并列 critical fanin）被跳过——移除一条不会降低 arrival。

### SAT-based 冗余检测

对每个候选 (pNode, iFanin)，调用 ABC 的 `Abc_NtkMfsSolveSatResub(p, pNode, iFanin, fOnlyRemove=1, fSkipUpdate=0)`：

1. 构建以 pNode 为中心的 MFS 窗口（TFO cone → roots → support → DFS nodes → divisors）
2. 将窗口编码为 AIG → CNF → SAT 问题
3. SAT 查询："在当前 don't-care 集合下，pNode 的功能是否依赖第 iFanin 个输入？"
4. 若 UNSAT（不依赖）：该 fanin 冗余，MFS 自动推导新函数并替换节点
5. 若 SAT（依赖）：该 fanin 不可移除，跳过

`fOnlyRemove=1` 保证**不会引入新的 fanin**——要么纯删除，要么放弃。这确保不会意外增加跨分区边。

### 迭代结构

```
for each round:
    SOP→AIG 转换（确保 Hop manager 一致性）
    计算 levels 和 reverse levels
    compute_arrival → extract_top_paths(K)
    收集 critical fanin 候选，计算权重，降序排列
    初始化 MFS manager
    对每个候选尝试 SAT-based 移除
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
  cmfs.cpp         — 核心实现
```

### 命令接口

```
usage: cmfs [-K num] [-R num] [-S num] [-C num] [-W num] [-F num] [-M num] [-v]
    -K num  : 分析的关键路径数量 [default = 16]
    -R num  : 最大优化轮数 [default = 20]
    -S num  : 停滞轮数限制 [default = 3]
    -C num  : 每次 SAT 冲突上限 [default = 5000]
    -W num  : MFS 窗口 TFO 层数 [default = 2]
    -F num  : MFS 窗口最大扇出数 [default = 30]
    -M num  : MFS 窗口最大节点数 [default = 300]
    -v      : 详细输出
```

### 典型使用流程

```
read design.v; strash; if -K 6; hpart -N 8; cpr; cmfs -v -K 32 -W 4 -M 500
```

### 实验结果（MCNC benchmarks）

在 29 个 MCNC benchmark 上测试（4/8/16 分区），算法能够找到并移除冗余边：

| Case | Partitions | Removals | Arrival Gain |
|------|-----------|----------|--------------|
| pdc  | 16        | 8        | 0            |
| alu4 | 16        | 5        | 0            |
| spla | 8         | 7        | 0            |
| pair | 16        | 2        | 0            |

算法正确移除了冗余 fanin，但**未产生 timing gain**。原因：在干净的 6-LUT mapping 之后，关键路径上的每条 edge 都是功能上必需的（SAT 证明）。能被移除的 edge 位于非关键路径上。

### 局限性与后续方向

1. **Pure removal 的固有限制**：post-mapping 网络中，mapper 已确保每个 LUT 的所有输入都是必要的。don't-care 自由度主要存在于非关键路径。

2. **Resub 模式（下一步）**：将 `fOnlyRemove=1` 改为 `fOnlyRemove=0`，允许用同分区 divisor 替换跨分区 fanin。这能把跨分区 edge 变成同分区 edge，直接减少 hop。需要加入 partition-aware 的 divisor 过滤逻辑。

3. **与 cpr 的协同**：cpr 的 replicate 阶段复制节点后可能创造新的冗余机会，可以在 cpr 之后立即运行 cmfs。

4. **Pre-mapping 应用**：在 AIG 阶段逻辑冗余更多，但需要不同的 timing 模型。

### 已知问题

- 少数 benchmark（frg2, x4）在 16 分区配置下仍有 crash，需进一步调查。
- 每轮需要 SOP→AIG 重新转换以保证 Hop manager 一致性，有一定开销。
