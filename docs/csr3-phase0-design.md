# csr3 Phase 0 — 组合 SDC 水分测量（设计 spec）

**日期**：2026-07-19
**状态**：设计已批准，待写实现计划
**方法论来源**：`docs/csr3.md`（完整的"割线即编码 / ACD 视角"方法论）
**本 spec 范围**：仅 Phase 0 的组合 SDC 测量路径（`docs/csr3.md` 的 Step 1-5 + Step 7），路线 A（SAT All-SAT）。

---

## 1. 目标与非目标

### 1.1 目标

一句话：**在 `hpart -N 2` 分区之后，只测量、不修改网表，用可信的数字回答"两个分区之间的跨界互联线里，组合层到底藏了多少可回收的冗余（水分）"。**

`csr`/`csr2` 逐边消 cut-edge，看不见"一束线的联合冗余"。`csr3` Phase 0 换信息编码视角：把一束同向跨界线看成一个多输出函数 `h: support → {0,1}^k`，问它的联合可达组合数 `m`，则该束的信息论下界是 `⌈log₂ m⌉` 位，`gain = k − ⌈log₂ m⌉` 就是组合层可回收的物理线数。

这个功能的**交付物是数据**：跑一遍回归套件，拿到"到底有没有水、水在哪"的第一手证据，据此决定要不要投入做 Phase 1（重编码引擎）。

### 1.2 非目标（本期明确不做）

- **不改网表**：Phase 0 是纯只读测量。encoder/decoder 综合、CEC、集成全部属于 Phase 1，不在本 spec。
- **不做时序不变式探针**（`docs/csr3.md` Step 6）：1-induction 证 one-hot/互斥需要时序归纳基础设施，工作量大一个量级，本期不做。**后果**：one-hot 总线这类"组合层恒盲"的场景会被读成 0 水分——报表口径必须写明是 **detected-floor（组合 SDC 下限）**，不是 zero。
- **不做 μ/ODC**（Phase 2）：不动 B 侧扇出锥，不做等价类合并。
- **不支持 N≠2**：v1 只处理两分区（partition 0 / 1），与 csr/csr2 的常用场景一致。N≠2 直接报错退出。
- **不建 TDM/SerDes/board-pair 模型**：FoxSYN 无此概念。gate 直接用原始可回收线数，不换算量化台阶。

---

## 2. 数据模型：什么是"束"

N=2，全局只有 partition 0 和 partition 1。cut-edge 是有向的（driver→consumer），所以"束"自然坍缩成**两个方向**，各自独立测量：

- **束 0→1** = { part=0 的节点 `n` : `n` 至少有一个 fanout 落在 part 1 }
- **束 1→0** = 对称（part=1 且至少一个 fanout 落在 part 0）

**束成员的物理单位是"一根跨界信号"（driver 节点 / net），不是 cut-edge。** 一个 driver 无论在对侧有几个 fanout，物理上都只占**一根**过界线，只算一次。因此 csr3 数的是 **cut-net 口径的过界信号数**——这与 `Pdb::cut_size()` 的 cut-net 口径一致，是"线"的正确物理单位（区别于 csr 优化时用的 cut-edge 口径，理由见 `docs/csr.md` §cut-edge vs cut-net）。

`k` = 一个组里的跨界信号线数，也是该组待测的编码位宽上界。

---

## 3. 处理流程

```
RunCsr3(pNtk, cfg):
    守卫: pNtk 非空 / Abc_NtkIsLogic / pNtk->pPdb 存在 / num_parts == 2

    for dir in {0->1, 1->0}:
        Step 1  crossing = collect_crossing_signals(pNtk, srcPart)   -- 收集该方向所有跨界 driver
        Step 2  for each line in crossing:
                    (cone, support) = extract_cone_partition_aware(line, srcPart)
        Step 3  groups = group_by_jaccard(crossing, threshold=cfg.jaccard, kmax=cfg.max_lines)
        Step 4  for each group:
                    aig = build_group_aig(group)                     -- MFS 式 window->AIG
                    if simulate_prefilter(aig, k) says ">2^(k-1)":   -- 仿真下界预筛
                        m = "no water"; continue
                Step 5  m = count_m_sat(aig, k, btlimit)             -- SAT All-SAT 精确数, 早退于 2^(k-1)
                        gain = k - ceil(log2(m))
        Step 7  emit_report(dir, groups)

    emit_global_summary()
```

### Step 1 — 收集跨界信号（`collect_crossing_signals`）

遍历 `Abc_NtkForEachNode`，对 `Abc_ObjGetPartId(pObj) == srcPart` 的节点，检查它是否存在一个**节点** fanout（`Abc_ObjIsNode`）落在对侧分区（`Abc_ObjGetPartId(fanout) == dstPart`，N=2 下即 `!= srcPart` 且有效）。命中即为一根该方向的跨界信号。PO/CO fanout 不参与判定（PO 无 partition，且与 `ComputeCutEdgeCount` 只计节点边的约定一致）——仅有 PO fanout 而无跨界节点 fanout 的 driver 不算跨界信号。

### Step 2 — partition-aware 抽锥（`extract_cone_partition_aware`）

**需新增**。ABC 现成的 `Abc_NtkNodeSupport` / `Abc_NtkDfsNodes` 会穿过对侧分区节点一直回溯到全局 CI，导致 support 过度扩张。csr3 要一个 partition 边界感知的 DFS：

从跨界信号 `line` 出发，向 fanin 方向 DFS，**停止边界**为：
1. PI（primary input）
2. FF 的 Q 端（latch output / CI）
3. 常量节点（`ABC_OBJ_CONST1`）
4. **对侧分区节点**：`Abc_ObjGetPartId(fanin) != srcPart` —— 把从对面来的信号当作本侧的自由输入叶子，不继续回溯

- `cone` = 从 `line` 到边界之间、`part == srcPart` 的内部组合节点集合
- `support` = 所有停止边界叶子的集合（PI / FF-Q / 常量 / 对侧节点）

用 ABC 的 TravId 标记避免重复访问。实现约 20-30 行，风格镜像 `ComputeCutEdgeCount` 的 part_id 守卫。

> **正确性（守恒律，`docs/csr3.md` §4.1）**：把所有叶子（含对侧来线）一律当**自由布尔变量**处理 ⇒ 允许的输入组合是真实可达集的**超集** ⇒ 算出的 `m` 只多不少（是真实 m 的**上界**）⇒ `gain = k − ⌈log₂ m⌉` 是真实可回收线数的**下界** ⇒ **永不过报**。漏掉的（真实 m 更小、上游约束更紧）留给未来的可达性/不变式探针。

### Step 3 — Jaccard 分组（`group_by_jaccard`）

联合冗余只存在于 support 重叠的线之间：两根 support 不相交的线，联合可达数 `m = m₁ × m₂`（乘积），放一组无收益。

1. 每根线的 support 存成 bitset（键为叶子 ObjId）。
2. 两两算 Jaccard `J = |support_i ∩ support_j| / |support_i ∪ support_j|`。
3. `J > cfg.jaccard`（默认 0.30）连边，求连通分量。
4. 每个连通分量再切成 `k ≤ cfg.max_lines`（默认 16）的小组（分量过大时按贪心/顺序切块；切块策略保守即可，不追求最优分组）。

单线组（k=1）也保留：若该线在 SDC 下恒为常量（m=1），gain=1，是一根可直接删的常量过界线，值得报。

### Step 4 — bit-parallel 仿真预筛（`build_group_aig` + `simulate_prefilter`）

**`build_group_aig`**：把一个组的联合锥构造成一个局部 AIG——联合 support 作为 AIG 的 CI，组里 k 根线作为 CO。参照 MFS 的 `Abc_NtkConstructAig` / 或用 `Abc_NtkToDar` 对锥子网构造。同一个 AIG 供 Step 4 和 Step 5 复用。

**`simulate_prefilter`**：bit-parallel 随机仿真（`Vec_Wrd`，每字 64 pattern，`cfg.sim_words` 字），对每个随机 support 赋值算出 k 位输出，hash set 数 distinct 组合。

- `distinct > 2^(k-1)` → **淘汰**（标记为"无水"，`gain=0`），不进 SAT。
- `distinct ≤ 2^(k-1)` → 放行进 Step 5。

> **正确性（`docs/csr3.md` §4.3）**：仿真数出的 distinct 是 m 的**下界**（没跑到的组合不会被数到）。`distinct > 2^(k-1)` ⇒ 真 m ≥ distinct > 2^(k-1) ⇒ 该组 `⌈log₂m⌉ = k`，gain=0，**淘汰永不误杀**。欠采样时可能把无水组**误留**进 SAT，由 Step 5 的早退兜住，只损失时间不损失正确性。

### Step 5 — SAT All-SAT 精确数 m（`count_m_sat`）

**这是唯一能报出可信水分的地方。** 仿真只给 m 下界（乐观），要断言"确有 N 根水分"必须精确数 m。

复用 Step 4 的局部 AIG → `Cnf_Derive` 得 CNF → All-SAT 循环（模板抄 `src/abc/src/sat/bmc/bmcClp.c`）：

```
count = 0
loop:
    status = sat_solver_solve(pSat, ...)
    if status == UNSAT: break
    读该组 k 个输出节点对应的 CNF 变量赋值，得到一个 k 位输出组合 y
    count++
    if count > 2^(k-1):   -- 早退：已证明无压缩价值
        m = ">2^(k-1)"; break
    加 blocking clause: 只禁这一个 k 位输出组合 y（对 k 个输出变量的补集析取）
m = count
```

关键：blocking clause 只投影到 **k 个输出变量**（不管 support 怎么取），所以枚举的是"不同的输出组合数"，正好是可达组合数 m。`cfg.btlimit` 限每次求解的回溯数。

> **成本随答案缩放**：m 小（真有水）→ 几步就 UNSAT，便宜；m 大（没水）→ `2^(k-1)` 早退，也不亏。两头都不失控。

### Step 7 — 报表（`emit_report` / `emit_global_summary`）

每组算 `gain = k − ⌈log₂ m⌉`（早退组 gain=0）。输出：

**每方向（0→1 / 1→0）**：
- 总跨界信号数（Step 1 命中数）
- 进组线数、组数
- Σk、Σ⌈log₂m⌉、**总可回收线数 Σgain**

**全局**：
- 两方向合计可回收线数
- 占总 cutsize 百分比（分母用 `Pdb::cut_size()` 或两方向 Σk）

**Top-N 组**（按 gain 降序，`-v` 下列出）：
- 每组 `k / m / gain / |support|`，定位水分集中在哪些束

**口径声明**：报表头部明确标注结果是 **detected-floor（组合 SDC 下限）**，非 zero；说明未计入可达性/ODC 水分（one-hot 等场景在组合层恒盲）。

---

## 4. 命令接口

镜像 `Csr_Command`（`src/main.cpp`）的 getopt 风格，单字符 switch + `argv[++i]` 消费数字参数。

```
usage: csr3 [-J num] [-M num] [-P num] [-B num] [-cv]
            Phase 0: measure combinational SDC "water" in cross-partition interconnect (read-only)
    -J num : Jaccard grouping threshold, percent (1-99) [default = 30]
    -M num : max lines per group (bundle size cap) [default = 16]
    -P num : random simulation words (x64 patterns each) [default = 16]
    -B num : SAT backtrack limit per solve [default = 100000]
    -c     : self-check — for groups with |support|<=16, brute-force exhaustive sim, assert == SAT's m
    -v     : toggle verbose (per-group detail + Top-N)
```

**守卫**（全部致命，打印错误并返回 1）：
- `!pNtk` → "csr3: current network is empty"
- `!Abc_NtkIsLogic(pNtk)` → "csr3: network must be logic (not AIG)"
- `!pNtk->pPdb` → "csr3: no partition database (run hpart first)"
- `num_parts != 2` → "csr3: v1 only supports N=2 partitions (got %d)"

命令函数 `Csr3_Command` 取网表用 `Abc_FrameReadNtk(pAbc)`，构造 `fox::csr3::Config`，调 `fox::csr3::RunCsr3(pNtk, cfg)`。**纯只读**，不改网表、不注册 undo。

---

## 5. 文件结构与构建

```
src/csr3/
  CMakeLists.txt   — 镜像 src/csr/CMakeLists.txt，链接 libabc + timer
                     （SAT/AIG/仿真全在 libabc；不需要链 cpr，因为不复用 balance 原语）
  csr3.hpp         — namespace fox::csr3 { struct Config; bool RunCsr3(Abc_Ntk_t*, const Config&); }
  csr3.cpp         — 编排 + 各单元函数
```

**单元函数划分**（每个职责单一、可独立理解）：
- `collect_crossing_signals(pNtk, srcPart) -> vector<Abc_Obj_t*>`
- `extract_cone_partition_aware(line, srcPart) -> {cone, support}`
- `group_by_jaccard(lines, supports, threshold, kmax) -> vector<Group>`
- `build_group_aig(group) -> Aig_Man_t*`（联合 support=CI，k 线=CO）
- `simulate_prefilter(aig, k, words) -> distinct_lower_bound`
- `count_m_sat(aig, k, btlimit) -> m`（All-SAT + 早退）
- `emit_report(dir, groups)` / `emit_global_summary()`

**构建接线**（三处）：
1. `src/CMakeLists.txt`：加 `add_subdirectory(csr3)`（挨着 `add_subdirectory(csr2)`）；把 `csr3` 加进 FoxSYN 可执行的 `target_link_libraries`。
2. `src/main.cpp`：加 `#include "csr3/csr3.hpp"`（挨着现有 csr/csr2 include）；在命令注册块加 `Cmd_CommandAdd(Abc_FrameGetGlobalFrame(), "FoxSYN", "csr3", Csr3_Command, 1);`；实现 `Csr3_Command`。

`Config` 是普通聚合体，字段带 in-class 默认值，置于 `fox::csr3` 命名空间；include `misc/util/abc_global.h` 与 `base/main/main.h`（镜像 csr.hpp/csr2.hpp）。

---

## 6. 正确性与验证

无单测框架（CLAUDE.md），按 `make` + 回归电路验证。三层：

### 6.1 内建自检（`-c`）

对 `|support| ≤ 16` 的组，bit-parallel **穷举** `2^|support|` 个输入向量——此时仿真是**完备**的，distinct = 精确组合层 ground truth。断言它等于 `count_m_sat` 的返回值 m。这直接验证 SAT 引擎 + CNF 变量映射 + blocking clause 逻辑的正确性。

### 6.2 不变式断言（always-on，`assert`）

每组恒满足：
- `1 ≤ m ≤ 2^k`
- `0 ≤ ⌈log₂ m⌉ ≤ k`
- `gain = k − ⌈log₂ m⌉ ≥ 0`
- `sim_distinct ≤ m`（仿真是下界）
- `|support| ≥ 1`（k=1 常量组的 support 可能来自单一叶子）

### 6.3 真实实验（交付目标本身）

```
read design.v; st; if -K 6; hpart -N 2; csr3 -v
```

跑回归套件（`scripts/` 下已有 csr 回归脚本可参照 `run_csr_regression.py`），产出"到底有没有水、水在哪些束"的第一批数据。这批数据就是本功能的交付物——据此决定是否立项 Phase 1。

---

## 7. 已知局限（写入报表口径）

- **detected-floor 而非 zero**：只测组合 SDC。可达性水分（如 one-hot 总线 `m=2^k`）、ODC 水分完全不计。报表说"至少有 N 根"，不说"只有 N 根"。
- **Jaccard 分组是启发式**：分组质量影响能发现多少联合冗余；support 完全不相交的线永远分不到一组（这是对的），但同一簇内的切块顺序会影响结果。保守方向：切块不会制造假水分（每组独立数 m，守恒律仍成立），只会漏发现跨块的联合冗余。
- **大 support 组的 SAT 成本**：cut 线的锥停在 FF/PI，联合 support 可能几十上百位。SAT 变量规模随之上升，但 All-SAT 早退（`2^(k-1)`，k≤16）把求解次数压在 32768 以内，且大 m 组会很快在仿真预筛阶段被淘汰，真正进 SAT 的应是少数小 m 组。
