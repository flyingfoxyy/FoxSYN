# csr4 — cut 函数上的 ACD：用列重数回收 B 侧的 ODC 冗余

> 这是 `csr`/`csr2`/`csr3` 之后的第四代割线优化思路，方法论 + 基本流程。
>
> `csr`/`csr2` 逐条消跨分区边（结构视角）；`csr3` 把一整束割线看成一个多输出函数的编码，用可达组合数 `m` 衡量它的浪费（**SDC** 视角）。`csr4` 换第三个视角：**不问 A 侧能产生多少种组合，问 B 侧能区分多少种组合**——在**已映射 LUT 网表**的 cut 函数上直接做 Ashenhurst-Curtis 分解，bound set 取跨界叶子，压缩宽度由**列重数 μ** 决定。这回收的是 **ODC** 水分，正是 `csr3` Phase 0 组合仪器恒读成 0 的那一块。
>
> 本文尚无实验数据。目的是把机制、记账纪律、可行性约束钉清楚，并说明为什么这条路比 `csr3` Phase 2 便宜一个量级。

---

## 目录

1. [背景：为什么 csr3 看不见这块水](#1-背景为什么-csr3-看不见这块水)
2. [一条负面定理：纯 cut 的 remapping 是 cut-neutral 的](#2-一条负面定理纯-cut-的-remapping-是-cut-neutral-的)
3. [核心洞察：cut 函数上的 ACD](#3-核心洞察cut-函数上的-acd)
4. [标杆例子](#4-标杆例子)
5. [记账纪律：三条会让数字虚高的账](#5-记账纪律三条会让数字虚高的账)
6. [覆盖条件：二部图连通分量](#6-覆盖条件二部图连通分量)
7. [分量超限时：把消不掉的 net 降级进 free set](#7-分量超限时把消不掉的-net-降级进-free-set)
8. [可行性约束](#8-可行性约束)
9. [Phase 0：只读测量流程](#9-phase-0只读测量流程)
10. [Phase 1：变换实现](#10-phase-1变换实现)
11. [与 csr3 的对比](#11-与-csr3-的对比)
12. [落地面：代码位置与已核实的事实](#12-落地面代码位置与已核实的事实)
13. [后续方向](#13-后续方向)
14. [术语表](#14-术语表)

---

## 1. 背景：为什么 csr3 看不见这块水

`csr3` Phase 0 对一束 k 根同向跨界线数**联合可达组合数 m**，报 `gain = k − ⌈log₂ m⌉`。它测的是 **SDC**（A 侧根本产生不出的组合）。

但水分有两层（`docs/csr3.md` §2.3）：`μ ≤ m ≤ 2^k`。`csr3` Phase 0 只做 `m`，把 `m → μ` 这一层（**ODC**，B 侧行为无差别、可合并的组合）整个划给了 Phase 2，并标注"要动 B 侧扇出锥、CEC 从 wire-level 放松到 output-level、贵一档"。

于是出现一类**恒盲**场景：A 侧 k 根线的所有 `2^k` 组合全部可达（`m = 2^k`，csr3 报 gain **0**），但 B 侧根本分不清其中大部分——真实需要的宽度是 `⌈log₂ μ⌉ ≪ k`。这不是罕见情形，它是"B 侧只在某个组合函数上使用这几根线"的通常形态。

`csr4` 的出发点：**在已映射的 LUT 网表上，这个 μ 可以被穷举精确数出来，而且需要的 encoder 是 mapper 顺手就能生成的一个 LUT，不需要 SAT、不需要 CEC。** ODC 水分在 cut 粒度上便宜得离谱。

---

## 2. 一条负面定理：纯 cut 的 remapping 是 cut-neutral 的

在设计 `csr4` 之前，有一条负面结果必须先记下来，因为它排除了一个看起来很自然、实际白干的方案。

那个方案是：用 `pst`（`src/pst/pst.cpp`）把已分区的 LUT 网表打散成**保持分区边界**的 AIG（每个 AND 带 `part_id`），然后 remapping，让 mapper 枚举 cut 时"尽量把跨分区边吃进 cut 内部"，最后给没有分区信息的 LUT 做 partition assign。

**定义**：一个 cut 是**纯的**，如果它覆盖的内部 AND 节点 `part_id` 全部相同；此时生成的 LUT 直接继承这个 `part_id`。

**命题**：若所有 cut 都纯且 LUT 继承其 `part_id`，则 cut-net 严格不变。

**证明**：取任意跨界 AND 边 `a(P0) → b(P1)`。`b` 是某个纯 P1 LUT `L_b` 的内部节点。若 `a` 也在 `L_b` 内部，则 `L_b` 同时含 P0 和 P1 节点，与"纯"矛盾。故 `a` 必是 `L_b` 的**叶子**，也就是说 `a` 自己是某个 LUT 的根，且该 LUT 属于 P0。于是跨界 AND 边一一对应跨界 LUT 边，两侧的跨界 net 集合严格对应。∎

**推论**：收益不可能来自"把 cut-edge 吃到 cut 内部"这个动作本身。不管 mapper 怎么重组 cut 边界，只要 cut 纯、part 继承，收益恒为 0。全部收益必须来自**不纯 cut + 重新 assign**，或来自**改变 cut 函数的实现方式**（这就是 `csr4` 走的路）。

**附带的一条负面事实**：也不能指望 pst-AIG 的结构里已经有需要的共享子函数。`Abc_NtkToAig` 走 SOP→AIG，是**分配**展开的：`r0 = a!b·x + !a·b·x` 建出来是 `or((a&!b)&x, (!a&b)&x)`，`x` 被推进每个乘积项，**没有** `a⊕b` 这个根节点。要在 AIG 上补出共享得靠 `rewrite`/`refactor`，而那会摧毁 `part_id`（`docs/pdecomp-design.md` 已记录这个张力）。

> 结论：**这个分解必须在 cut 的真值表层面做。**

---

## 3. 核心洞察：cut 函数上的 ACD

取 B 侧（P1）一个 LUT `L`，把它的叶子按分区切成两块：

```
bound set  B_L = { L 的叶子 : part_id ≠ P1 }      ← 跨界叶子
free set   F_L = { L 的叶子 : part_id  = P1 }      ← 本地叶子
```

对 `B_L` 的每个赋值 `b`，`L` 的函数退化成一个只关于 `F_L` 的子函数（**restriction**）：

```
r_b : F_L → {0,1}
```

**列重数 μ = 不同的 `r_b` 个数**。若 μ 小于 `2^{|B_L|}`，说明 B 侧**分不清**那些给出相同 `r_b` 的 `b`——它们是可合并的等价类，多余的区分能力就是 ODC 水分。信息论下界是 `t = ⌈log₂ μ⌉` 位：

```
gain = |B_L| − t          （可回收的物理线数）
cost = t 个新 LUT（在源侧 P0，作 encoder）
```

变换形态：A 侧新增 encoder `h : B_L → {0,1}^t`（把 `b` 映到它的类编号），`L` 改成读 `t` 根码线加上原有本地叶子。**B 侧不需要 decoder**——`L` 的新函数 `g_L(t, F_L)` 直接把 `g` 吸收进 LUT 自己的真值表里。这是相对 `csr3` 最省的地方。

对号入座到经典 ACD（`docs/csr3.md` §3）：

```
bound set  X_b  =  L 的跨界叶子 B_L
free set   X_f  =  L 的本地叶子 F_L
列重数     μ    =  不同 restriction 数（穷举可数，不需要 SAT）
h              =  A 侧新增的 encoder LUT
g              =  被 L 的新真值表吸收，不产生独立逻辑
```

**关键便利**：LUT 真值表只有 `2^K` 位，K=6 就是**一个机器字**。`docs/csr3.md` §Step 9 里那句"`support → t` 要 `2^|support|` 会爆，所以只能在 `k↔t` 这一层做"的顾虑，在 cut 粒度**完全不存在**。μ 是穷举数出来的**精确值**，不是 SAT 估的界。

**功能正确性由分解本身保证**：`g_L` 良定义（见 §6 的公共细化论证），不需要 miter / CEC 来证等价。这与 `csr3` Phase 1 必须做 cone-level CEC（`docs/csr3.md` Step 10）形成对比。工程上仍可跑一次 `cec` 作廉价保险，但它不是正确性的必要环节。

**hop 中性**：`b(P0) → enc(P0) → L(P1)` 与 `b(P0) → L(P1)` 的跨分区次数相同。`Abc_NtkComputeHopNum`（`src/abc/src/base/abc/abcPdb.cpp:218`）数的是分区变化次数，不是逻辑层数，所以 encoder 不增加 hop。

---

## 4. 标杆例子

K=4 FPGA。A 侧（P0）有 `a, b`；B 侧（P1）有本地信号 `x, y`，需要计算：

```
r0 = (a ⊕ b) ∧ x
r1 = (a ⊕ b) ∨ y
```

**普通 area-oriented mapper 的结果**：两个 3 输入 LUT，都放在 P1（因为 `x, y` 在 P1）：

```
P0                    P1
 a ──┬──────────────→ LUT0(a, b, x) ──→ r0
     └──────────────→ LUT1(a, b, y) ──→ r1
 b ──┬──────────────→ LUT0
     └──────────────→ LUT1
 x ────────────────→ LUT0
 y ────────────────→ LUT1

cut-net = |{a, b}| = 2
```

mapper 认为这个结果很好：2 个 LUT、深度 1、每个函数都装得进 4-LUT。**面积和深度目标函数不会主动改变它。**

**csr4 的视角**。`LUT0`：`B = {a,b}`、`F = {x}`，列出 restriction：

```
ab = 00 → r = 0        ab = 01 → r = x
ab = 11 → r = 0        ab = 10 → r = x
                       μ = 2,  t = 1
```

`LUT1`：`B = {a,b}`、`F = {y}`：

```
ab = 00 → r = y        ab = 01 → r = 1
ab = 11 → r = y        ab = 10 → r = 1
                       μ = 2,  t = 1
```

两者在 `B` 上诱导出**完全相同的划分** `{00,11} | {01,10}`，所以共享同一个 encoder：

```
P0                         P1
 a ──┐
     ├─ ENC(a,b) ─ t ──┬─→ LUT0'(t, x) ──→ r0
 b ──┘                 └─→ LUT1'(t, y) ──→ r1
 x ──────────────────────→ LUT0'
 y ──────────────────────→ LUT1'

t  = a ⊕ b
r0 = t ∧ x
r1 = t ∨ y

cut-net: 2 → 1
```

**代价**：新增 1 个 LUT（面积 2→3），该路径深度 1→2。emulator 的成本比下（线极贵、LUT 极便宜）这笔换算很划算，但注意 **mapper 的面积目标函数会主动反对它**——这是 Phase 1 走 remapping 路线时必须解决的问题（见 §13）。

**这正是 csr3 Phase 0 恒盲的场景**：`{a,b}` 四种组合全部可达，`m = 4 = 2^k`，csr3 报 gain 0；而 μ = 2。纯 ODC 水分。

---

## 5. 记账纪律：三条会让数字虚高的账

这三条是本方法最容易出错的地方，全部会导致**过报**。

**（1）必须按 net 记账，不能按 LUT 求和。** 同一根跨界 net `a` 被 B 侧三个 LUT 用到，三个 LUT 各自报 gain，一加就三倍虚报——物理上 `a` 只有一根线。记账单位必须是"**一组共享 encoder 的 LUT**"，gain 数的是被消掉 net 集合的**基数**，不是各 LUT gain 之和。

**（2）all-sinks 条件。** 一根跨界 net `a` 真正停止过界，前提是它在 B 侧的**所有** fanout 都被这次变换覆盖（都改读 `t`）。漏掉一个，`a` 仍然是 LUT 根、仍然要过界，那一份 gain 归零。

> 注意这**不等于**"只适用于 sink 数为 1 的 cut-edge"。sink 数为 1 只是条件平凡成立的特例。§4 的例子里 `a`、`b` 在 B 侧各有 2 个 sink，照样双双消掉——**多 sink 不是障碍，它恰恰是共享 encoder 的动机**。分组不是锦上添花的优化，它是满足 all-sinks 条件的**机制本身**。

**（3）共享 encoder 要算 joint μ，不能用单个 LUT 的 μ 代表整组。** 一组 LUT 共用一个 encoder 时，encoder 必须同时区分**每个** LUT 需要区分的 `b`，也就是各 LUT 诱导划分的**公共细化（meet）**。算法上：对 `b` 的每个赋值，把组内每个 LUT 的 restriction **拼接**成一个 key，数不同 key 数。这等价于对多输出函数算列重数。

**sink 判定口径**：跟 `Abc_NtkComputeCutEdgeNum`（`src/abc/src/base/abc/abcPdb.cpp:181`）走——只算带有效 `part_id` 的节点 fanout，PO/CO 不参与（PO 无分区）。这与 `csr3` Phase 0 `collect_crossing_signals` 的约定一致。

---

## 6. 覆盖条件：二部图连通分量

all-sinks 条件有一个自然的算法形式，不需要额外判定。

建二部图：一边是跨界 net，一边是 B 侧消费它们的 LUT，net 向它的每个 B 侧 sink LUT 连边。

取一个**连通分量**。由连通性定义，分量里每个 net 的所有 sink LUT 都在分量内——所以**分量内的 net 全部满足 all-sinks 条件**。这就是需要的闭包，免费得到。§4 的例子里 `{a, b, LUT0, LUT1}` 正好构成一个分量。

**joint μ 的良定义性**（为什么 `g_L` 存在）：设分量的 bound set 为 `B`，encoder 按**公共细化**给每个 `b` 分配码字 `t`。对组内 LUT `L`，它自己的划分比公共细化**粗**，所以"公共细化的类"→"`L` 的类"是良定义的映射，于是 `r_b` 只通过 `t` 依赖 `b`，`g_L(t, F_L)` 良定义。这正是**必须**用公共细化（而不是各自的 μ）的原因。

某个 LUT 不依赖 `B` 的某些位是无所谓的——那几位在它的 restriction 上恒定，不影响拼接。

---

## 7. 分量超限时：把消不掉的 net 降级进 free set

传递闭包容易膨胀，分量可能很大，而算 joint μ 要枚举 `2^{|B|}`，撑不住就得截断分组。截断之后，有些 net 的 sink 落在组外，这些 net 照样过界。

正确的处理**不是**把它们算作亏损，而是**把它们从 bound set 挪到 free set**：它们反正会过界，B 侧本来就拿得到，当自由变量用不花钱。

```
B_kill = 组内 sink 全覆盖的跨界 net        ← 参与压缩
B_keep = 其余跨界 net  →  并入 free set    ← 反正会过界，白用

μ_joint 在 B_kill 上算（bound set 更小 ⇒ μ 只会更小）
gain = |B_kill| − ⌈log₂ μ_joint⌉
```

这个降级同时让**账变准**（不虚报 B_keep）和**gain 变大**（bound set 更小、μ 更小）。此时 LUT `L` 的有效 free set 是 `F_L ∪ (B_keep ∩ leaves(L))`。

---

## 8. 可行性约束

**（1）目的侧 K 约束，必须逐 LUT 检查。**

单 LUT 分组时天然可行：`B_L ⊆ leaves(L)`，新叶子数 `= |leaves(L)| − |B_L| + t ≤ |leaves(L)| ≤ K`（因为 `t ≤ |B_L|`），**只会变少**。

但联合分组时**不成立**。组里的 LUT 各自只用 `B_kill` 的一个子集，而它要读的是完整的 `t`。反例：`B_kill` 有 10 根，μ=512 故 t=9；某个 `L` 只用其中 2 根加 4 个本地叶子，改读 `t` 后是 9+4=13 个叶子，K=6 装不下。

所以要逐 LUT 验：

```
t + |F_L^eff| ≤ K        对组内每个 L
```

这给出分组的真实张力：**组越大越省 encoder（共享），但 t 增长会把小 LUT 挤爆。**

**（2）encoder 侧 K 约束。** encoder 的输入是 `B_kill`。单 LUT 分组时 `|B| ≤ K`，一个 LUT 装得下；联合分组时 `|B_kill|` 可以超过 K，encoder 本身需要多级 LUT 分解（或该组不可行）。这一项在成本核算里不能忽略。

**（3）与"吸收"方向的对比。** 上一代思路里那个"把跨界 LUT 吃进消费侧 LUT"的动作被 K 死死卡住（吃不下 32 位 one-hot 总线）。`csr4` 没有这个上限——k=32 的束只要 μ 小，一样能压。**这是两个不同的机会，不要混为一谈。**

---

## 9. Phase 0：只读测量流程

沿用 `csr3` 那套"测量先于实现"的 gating（`docs/csr3.md` §5），它证明过是省事的。测量**完全不动 mapper、不用 pst**，直接在现有 LUT 网表上跑（`if -K 6; hpart -N 2` 之后），纯只读。

```
Step 1  收集跨界 net：遍历 LUT，按 Abc_NtkComputeCutEdgeNum 的口径判定
Step 2  对每个 LUT 求 B_L = { 叶子 : part_id ≠ 本 LUT part_id }
        丢掉 |B_L| ≤ 1 的（不可能有收益）
Step 3  建 net↔LUT 二部图，求连通分量做分组
        分量超 cap 时截断，未覆盖的 net 降级为 B_keep（§7）
Step 4  每组算 joint μ：对 B_kill 的每个赋值，把组内各 LUT 的 restriction
        拼成 key 塞 hash set，数不同 key 数。2^|B_kill| 次迭代
Step 5  逐 LUT 验 K 约束（§8），不可行则缩小分组重试或放弃该组
Step 6  gain = |B_kill| − ⌈log₂ μ_joint⌉，非正则丢弃
Step 7  报表：总 gain 线数、需要的 encoder LUT 数、按 gain 排序的组清单
```

复杂度：网表规模线性 × `2^K` 常数。秒级。

**真值表来源**：`if` 默认输出 `ABC_NTK_LOGIC / ABC_FUNC_AIG`（`src/abc/src/base/abci/abcIf.c:328`），用 `Hop_ManConvertAigToTruth`（`src/abc/src/aig/hop/hop.h:329`）；或先 `Abc_NtkToSop` 再 `Abc_SopToTruth`（`src/abc/src/base/abc/abc.h:959`，K≤6 直接返回一个 `word`）。

**报表口径（沿用 csr3 的 detected-floor 措辞）**：结果是**下限**，不是"只有这么多"。

- 只看**当前 cut 边界**。mapper 有额外自由度（选不同 cut 让跨界 bound set 更窄），真实可得只会更多。
- v1 的分组是启发式（分量截断顺序影响结果），跨组的共享机会漏掉。
- 说"至少 N 根"，不说"只有 N 根"。

**gate**：如果回归集上这个数是个位数百分比，那 §13 的 mapper 大改就不值得做；这个否定结论本身就是成果。

---

## 10. Phase 1：变换实现

Phase 0 报出正收益后才做。逐组独立、天然并行、失败跳过。

```
Step 8   码字分配：给 μ_joint 个等价类各分配一个 t 位码字
         未用码字（2^t − μ）全部当 don't care 喂给两侧综合
         机会主义匹配（沿用 docs/csr3.md Step 9）：先问"P0 网表里是否已存在
         一个信号恰好等于某个码位"，命中就直接引线、零逻辑
Step 9   在 P0 侧综合 encoder（t 个 LUT，输入 B_kill；|B_kill| > K 时需多级分解）
Step 10  重写组内每个 LUT 的真值表为 g_L(t, F_L^eff)，改接叶子
Step 11  更新 Pdb：encoder LUT 打 P0 的 part_id
         断言 cut-net 下降、hop 不增、balance 仍满足
         （可选）跑一次 cec 作廉价保险——正确性由分解保证，非必需
```

**代价关卡**：若 encoder 的 LUT / 深度代价超过省下的线的价值，放弃该组。emulator 里线极贵，通常不触发，但门要留着。

**balance**：encoder 全部落在 P0，会单方向增加 P0 的 LUT 数。大批量应用时要核算平衡约束。

---

## 11. 与 csr3 的对比

| 维度 | `csr3` | `csr4` |
|---|---|---|
| 视角 | A 侧能产生多少种组合 | B 侧能区分多少种组合 |
| 回收的水 | SDC（`m`），Phase 2 才做 ODC | **ODC**（`μ`），直接做 |
| 粒度 | 一束跨界线 + 它们的锥（几百 LUT） | 单个 cut 函数（≤ `2^K` 位真值表） |
| μ / m 怎么得 | SAT All-SAT 枚举 + 早退（近似/带界） | **穷举精确数**，一个机器字 |
| bound set | A 侧 support（几十上百位，要 `2^k` 层做） | LUT 的跨界叶子（≤ K） |
| encoder | 需要综合 `k→t` 压缩器 | 一个 LUT（或多级，`|B_kill|>K` 时） |
| decoder | **需要**，B 侧 `t→k` 解压器 | **不需要**，被 LUT 真值表吸收 |
| 正确性验证 | **必须** cone-level CEC（含锥的 miter） | 由分解保证，CEC 只是保险 |
| 上限 | 可压很宽的束（k≤16 起步） | 受目的侧 K 约束（§8） |

两者**正交、可叠加**：`csr3` 压的是"束在 A 侧的编码宽度"，`csr4` 压的是"cut 函数对跨界叶子的依赖宽度"。同理与 `cpr`（hop）、`cmfs`（arrival）、`csr`/`csr2`（逐边）都不冲突。

---

## 12. 落地面：代码位置与已核实的事实

以下均已在当前仓库核实：

- **`part_id` 完全不过 mapper**。`Abc_NtkToIf`（`src/abc/src/base/abci/abcIf.c:208`）只建 Abc→If 的 `pCopy`；`Abc_NtkFromIf` / `Abc_NodeFromIf_rec`（同文件 `:315` / `:565`）走 `Abc_NtkCreateNode` 建全新节点，没有任何 `part_id` 传递。Phase 0 不需要碰它；§13 的 remapping 路线需要。
- **ABC 自带的 ACD 代码不能直接复用**。`src/abc/src/map/if/acd/ac_decomposition.hpp:214` 那套 `column_multiplicity` 有 `static_assert(free_set_size <= 3)`，而且 free set 固定是**低位变量**、bound/free 约定与我们相反（我们的 bound set 是分区决定的任意位置子集）。K≤6 自己写行计数二十行就够，比适配它省事。
- **cut 剪枝是 remapping 路线的主坑**。`If_ObjPerformMappingAnd`（`src/abc/src/map/if/ifMap.c:226`）算完 Delay/Area 后按 `If_CutCompareDelay`/`If_CutCompareArea`（`src/abc/src/map/if/ifCut.c:408` / `:468`）排序并截断到 `nCutsMax`。一个"值得插 encoder"的 cut 在面积上是**劣势**的（§4），会在被挑到之前就剪掉。partition 项必须进**比较器本身**，且要在 area-flow 各轮迭代间保持一致，否则震荡。
- **`pst` 已经能保分区边界打散成 AIG**（`src/pst/pst.cpp`），用 hop 相等做不变量断言，跨分区 AND 合并强制建副本（`dup_blocked`）。已知限制：单 fanin LUT 在 AIG 里塌掉、常量 LUT 落到共享 const1 拿不到 `part_id`、不支持在 `pdecomp` 之后跑。`csr4` Phase 0 用不到它。
- **`hpart` 目前不传 hmetis 的 FixFile**（`src/hpart/hpart.cpp:245` 的 `BuildCommand` 走不带 fixfile 的 9 参数形式）。若将来需要"带固定顶点的重划分"（§13），这是个小改动，且已有 `--load-part` 旁路。

---

## 13. 后续方向

- **联合分组策略**：v1 用"相同 `B`"或"连通分量 + 截断"做键。更好的分组要在"共享 encoder 省下的 LUT"和"t 增长挤爆小 LUT"（§8）之间做权衡，是个真正的优化问题。
- **partition-aware remapping（v2）**：让 mapper 在**枚举 cut 时**就把 `csr4` 的 gain 算进代价函数，而不是事后在固定 cut 边界上找机会。额外能力是可以选择让跨界 bound set 更窄的 cut 边界（Phase 0 的下界口径正来自此）；代价是 §12 那个剪枝坑，以及 pst-AIG 结构偏置带来的面积/深度回退（不能跑 `rewrite` 洗偏置，那会摧毁 `part_id`）。
- **锚点 balance 可行性**：若走 remapping + partition assign 路线，原分区是在**旧 LUT 网表**上按节点数配平的，重映射后各分区 LUT 数任意变化，硬钉锚点可能直接 balance 不可行。倾向的出路是**把继承的 `part_id` 当 warm start，跑带 balance 约束的 FM refinement**（锚点也允许动），而不是硬钉——硬钉还会堵死本来有利的重定位。
- **公平 baseline**：若 "after" 是一个**不同的** LUT 网表，跨网表比 cut 数正是 `pdecomp` 当初为解耦而造出来的那个混杂变量。对照组必须是 **"partition-oblivious remap + 从头重划分"**，不是原始网表。诚实的声明形式是"cut 降 X%，代价是面积/深度 +Y%"。
- **N > 2**：本文按 N=2 的 A/B 两侧叙述。多分区时一根 net 可能去多个目的分区，bound set 和 sink 覆盖都要按 `(源, 目的)` 方向拆开处理（沿用 `docs/csr3.md` §9.3 的独立处理约定）。
- **与 csr3 联动**：`csr3` 压完 SDC 之后再跑 `csr4` 收 ODC，或反之。两者对同一束线的作用正交，迭代顺序值得实测。

---

## 14. 术语表

| 术语 | 含义 |
|---|---|
| **cut / cutsize** | 跨分区的连线集合 / 其数量 |
| **cut-net vs cut-edge** | net：一条 net 有任意 fanout 跨区就记一次；edge：按 `(driver, distinct 目的分区)` 去重计数 |
| **纯 cut** | 覆盖的内部节点 `part_id` 全部相同的 cut；生成的 LUT 可直接继承该 `part_id` |
| **bound set / free set** | ACD 中被压缩的变量集 / 直接透传的变量集；此处 = 跨界叶子 / 本地叶子 |
| **restriction `r_b`** | 固定 bound set 赋值 `b` 后，只关于 free set 的子函数 |
| **列重数 μ** | 不同 restriction 的个数；决定压缩宽度下界 `⌈log₂ μ⌉` |
| **公共细化（meet）** | 多个划分的最粗公共细化；共享 encoder 必须按它编码，`g_L` 才良定义 |
| **`B_kill` / `B_keep`** | 组内 sink 全覆盖、参与压缩的跨界 net / 其余跨界 net（降级进 free set） |
| **all-sinks 条件** | 一根 net 停止过界的前提：它在目的侧的全部 fanout 都被本次变换覆盖 |
| **SDC / ODC** | 可满足性无关项（永不出现的组合）/ 可观测性无关项（无人区分的组合） |
| **ACD** | Ashenhurst-Curtis Decomposition，按 bound/free set 分解、以最少中间连线实现 |
| **encoder / decoder** | 源侧压缩器 `B→t` / 目的侧解压器；`csr4` 不需要 decoder |
| **detected-floor** | 报表口径：探到的下限，非"真没有"。沿用 `docs/csr3.md` §4.2 |
| **TDM** | Time-Division Multiplexing，多根逻辑割线分时复用一根物理线 |

---

## 相关文档

- `docs/csr.md` — 第一代 cut-edge reducer（resub + replication，逐边视角）
- `docs/csr2.md` — 第二代（Frame 事务模型、多轨迹搜索）
- `docs/csr3.md` — 第三代（割线即编码，SDC / `m` 视角）
- `docs/csr3-phase0-design.md` — csr3 Phase 0 只读测量 spec，本文 Phase 0 的工程参照
- `docs/pst-design.md` — 分区保持的 structural hashing，remapping 路线的前置
- `docs/pdecomp-design.md` — 分区保持的 LUT 分解；跨网表比较的混杂变量讨论
- `docs/hpart.md` — 分区数据来源
