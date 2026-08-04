# fmpart — 模板化 FM 二分划分器（设计 spec）

**日期**：2026-08-04
**状态**：设计已批准，待写实现计划
**算法来源**：Fiduccia & Mattheyses, *A Linear-Time Heuristic for Improving Network Partitions*, DAC 1982

---

## 1. 目标与非目标

### 目标

在 `src/fmpart/` 下提供一个 header-only 的模板类 `FMPart<G>`，对任何满足 `FMHypergraph` concept 的图类型做 FM 二分划分，最小化 cut-net 权重和。支持：

- **平衡约束**：两侧顶点权重和不超过上界
- **FixNode 约束**：指定顶点钉死在某一侧，永不移动
- **模板化**：`FMPart<AbcNtkWrapper>` 与 `FMPart<SimpleHypergraph>` 用同一份实现

本次交付包含 `AbcNtkWrapper` 适配器，用来证明模板确实能套在两种不同的图上，并让 FM 能直接吃真实网表。

### 非目标

- **不做多级划分**（coarsening / initial partitioning / uncoarsening）。本 spec 只覆盖单层 refinement。
- **不做 k-way**。只做二分。k 路留给后续的递归二分驱动，那时不需要改 `FMPart` 本体。
- **不注册 ABC 命令，不写回 `Pdb`**。`AbcNtkWrapper` 会暴露 `vertex_to_obj()`，把回写留给调用方。
- **不改 `src/hpart/`**。见 §7。

---

## 2. 现有基础设施

| 位置 | 内容 | 与本设计的关系 |
|---|---|---|
| `src/hpart/hpart.cpp:164` | `BuildHypergraph()`：driver + 传递可达 sink = 一条超边 | `AbcNtkWrapper` 采用同一建图口径，cut 数才可比 |
| `src/abc/src/base/abc/abcPdb.hpp` | `Pdb`：obj id → `part_id`（uint8，上限 255 分区） | 本次不写回，仅预留 `vertex_to_obj()` |
| `src/cpr/cpr.cpp:254` | `compute_balance_max_allowed()` | 平衡上界公式照抄，保证与 `cpr`/`csr`/`hpart` 同口径 |
| `abcPdb.cpp:173` | `Abc_NtkComputeCutSize()`：cut-net 计数 | FM 的 cut 度量与之等价（2-way 下 cut-net = λ−1） |
| `regression/SimpleCircuits/mcnc/*.v` | 真实电路 | 真实电路测试输入 |
| `patoh` @ `/home/longfei/HyperPar-main/tools/patoh` | 多级超图划分器 | 提供 2-way cut 参考值 |

---

## 3. 对外接口

### 3.1 concept

```cpp
template <typename G>
concept FMHypergraph = requires(const G &g, int v, int e) {
    { g.num_vertices()   } -> std::convertible_to<int>;
    { g.num_nets()       } -> std::convertible_to<int>;
    { g.vertex_weight(v) } -> std::convertible_to<int>;
    { g.net_weight(e)    } -> std::convertible_to<int>;
    { g.pins_of(e)       } -> std::ranges::input_range;
};
```

**契约**（concept 无法表达，写在注释里）：

- 顶点 id 连续覆盖 `[0, num_vertices())`，net id 连续覆盖 `[0, num_nets())`
- `pins_of(e)` 产出的元素可转成 `int`，且都是合法顶点 id
- `pins_of(e)` 内部不含重复顶点（适配器负责去重）
- `vertex_weight(v) >= 0`，`net_weight(e) >= 0`
- 所有成员在 `FMPart` 构造期间可重入调用；构造之后 `FMPart` 不再触碰 `G`

**刻意不要求 `nets_of(v)`**：`FMPart` 构造时把 `pins_of` 转置一次即可得到，少一项适配器负担。

### 3.2 Config / Result

```cpp
struct Config {
    int      balance_pct = 2;    // 平衡松弛百分比，语义同 cpr.cpp:254
    int      max_passes  = 10;   // pass 数上限
    int      min_gain    = 1;    // 一趟收益 < min_gain 即收敛退出
    unsigned seed        = 1;    // 随机初始解种子
    bool     verbose     = false;
};

struct Result {
    std::vector<uint8_t> part;        // 每个顶点所属分区，0 或 1
    int  cut         = 0;             // 最终 cut：被切开的 net 权重和
    int  initial_cut = 0;             // 优化前的 cut
    int  passes      = 0;             // 实际执行的 pass 数
    bool balanced    = false;         // 最终解是否满足平衡约束
};
```

### 3.3 调用形式

```cpp
AbcNtkWrapper g(pNtk);
FMPart<AbcNtkWrapper> fm(g, cfg);

Result r1 = fm.run();                  // 随机平衡初始解，无固定点
Result r2 = fm.run(init, fixed);       // 指定初始解与固定点
```

```cpp
Result run(std::span<const uint8_t> init  = {},
           std::span<const int8_t>  fixed = {});
```

- `init` 为空 → 用 `cfg.seed` 生成一个尊重 `fixed` 的随机平衡初始解；否则长度必须等于 `num_vertices()`，元素为 0/1
- `fixed` 为空 → 无固定点；否则长度必须等于 `num_vertices()`，`-1` 表示自由，`0`/`1` 表示钉死在该侧
- `init` 与 `fixed` 冲突时（`fixed[v] >= 0` 且 `init[v] != fixed[v]`），以 `fixed[v]` 为准
- 长度不匹配 → `assert` 失败（这是调用方 bug，不是运行期错误）

用单个 `int8_t` 数组同时表达「是否固定」与「固定到哪侧」，比两个平行数组少一处可能不同步的状态。

`run()` 可以在同一个 `FMPart` 实例上重复调用，每次独立（CSR 快照复用，FM 状态重置）。这是把约束放在 `run()` 而非 `G` 上的直接收益：多次随机重启取最优，不必重建图。

---

## 4. 内部结构

### 4.1 一次性 CSR 快照

构造函数遍历一次 `pins_of(e)`，落成两组 CSR：

```
pin_start[e], pin_list[]    // net -> 顶点
net_start[v], net_list[]    // 顶点 -> net（由上者转置得到）
```

同时缓存 `vw[v]`、`nw[e]`。之后所有内层循环只碰扁平数组，`G` 的访问性能不再影响 FM 性能，适配器可以写得很朴素。

代价：约 `2 × pins` 个 int。100 万 pin 约 8 MB，可以接受。

### 4.2 FM 状态

| 数组 | 含义 |
|---|---|
| `part[v]` | 当前所属分区，0 或 1 |
| `cnt[e][0]`, `cnt[e][1]` | net `e` 在两侧各有多少 pin |
| `gain[v]` | 当前增益 |
| `locked[v]` | 本趟是否已锁定（固定点恒为 true） |
| `wsum[0]`, `wsum[1]` | 两侧顶点权重和 |

cut 的定义（全文统一）：

```
cut = Σ { nw[e] | cnt[e][0] > 0 且 cnt[e][1] > 0 }
```

### 4.3 增益定义

对顶点 `v`，记 `F = part[v]`，`T = 1 - F`：

```
gain(v) = FS(v) - TE(v)
FS(v) = Σ { nw[e] | e ∋ v, cnt[e][F] == 1 }   // v 独占己方 → 移走就解开这条 net
TE(v) = Σ { nw[e] | e ∋ v, cnt[e][T] == 0 }   // 对面为空 → 移过去就切开这条 net
```

`gain(v)` 恰好等于「把 v 移到对面后 cut 的减少量」。

### 4.4 增益桶

每侧一个桶数组，下标 `gain + Gmax`，长度 `2·Gmax + 1`，其中

```
Gmax = max_v Σ_{e ∋ v} nw[e]
```

桶内用侵入式双向链表（`bnext[v]`, `bprev[v]`，全局数组，无分配）。维护 `max_gain[side]` 指针，取最大时若当前桶空则向下扫描（懒下降）。

- 取最大：O(1) 摊还
- 插入 / 删除 / 改增益：O(1)

网表里 `Σ_{e ∋ v} nw[e]` 约等于 `1 + fanin(v)`，`Gmax` 很小。即便退化到 `Gmax = num_nets()`，桶数组也只是 `O(num_nets())` 个指针，不构成问题。

桶结构独立成 `fm_buckets.hpp`，可单独测试。

---

## 5. 算法

### 5.1 平衡上界

```
total      = Σ vw[v]
avg        = total / 2
max_weight = max(avg + ceil(avg * balance_pct / 100), avg + 1)
```

与 `cpr.cpp:254` 完全一致。顶点权重全为 1 时退化成「节点数」口径，可直接与 `patoh` / `hpart` 的结果对比。

因为 `max_weight >= total / 2`，**两侧不可能同时超重**。这条性质让 §5.3 的强制出侧逻辑不必处理双侧超重的情形。

### 5.2 移动可行性

移动 `v: F → T` 可行，当且仅当：

```
wsum[T] + vw[v] <= max_weight
```

不额外允许「减少违规量」的移动。理由：由 §5.1 的性质，超重侧的顶点移向另一侧时目标侧必然低于 `max_weight`（除非单个 `vw[v]` 大到跨越整个松弛区间，那属于输入病态，如实反映在 `Result::balanced` 上即可）。多一条特例规则会让回滚时的可行性判断变复杂，收益不抵成本。

### 5.3 一趟 pass

```
解锁所有非固定顶点，从当前 part[] 重算 gain[]，全部入桶
cum = 0; trail.clear()
best_prefix = 0; best_key = key(当前状态, 0)

循环 直到无可移动顶点:
    选择出发侧 side:
        若 wsum[0] > max_weight        -> side = 0      (强制)
        否则若 wsum[1] > max_weight    -> side = 1      (强制)
        否则 -> 取两侧桶顶增益较大者；相等则取权重较重的一侧
    从 bucket[side] 顶端向下找第一个满足 §5.2 的顶点 v；找不到则:
        若 side 是强制的 -> 退出循环
        否则 -> 换另一侧再试一次，仍找不到则退出循环

    执行移动 v: side -> 1-side
    locked[v] = true; 从桶中摘除
    cum += gain[v]
    按 §5.4 更新 cnt[] 与邻居 gain[]
    trail.push_back(v)

    若 key(当前状态, cum) > best_key:
        best_key = key(当前状态, cum); best_prefix = trail.size()

回滚 trail 中 best_prefix 之后的所有移动
返回 best_prefix 处的 cum
```

**前缀比较键**：

```
key(状态, cum) = (wsum[0] <= max_weight 且 wsum[1] <= max_weight,  cum)
```

按字典序比较：**先比是否平衡，再比累积增益**。

这一层不能省。若只按 `cum` 比较、并把初值设为 `cum = 0`，那么当起点本身违反平衡时（用户给的 `init` 叠加固定点完全可能造成），FM 会回滚到那个不平衡的起点，哪怕本趟途中经过了一个 `cum` 只低一点的平衡状态。加上这一层后，只要本趟出现过平衡状态，回滚就一定落在平衡状态上。

若整趟都没出现平衡状态、起点也不平衡，则退化为纯按 `cum` 取最优，最终 `Result::balanced` 报 `false`。

回滚必须同样更新 `cnt[]`、`wsum[]`、`part[]`；`gain[]` 不必回滚，因为下一趟开头会整体重算。

**驱动循环**：

```
initial_cut = compute_cut()
for p in 1..max_passes:
    g = run_one_pass()
    passes = p
    if g < min_gain: break
cut = compute_cut()
balanced = (wsum[0] <= max_weight && wsum[1] <= max_weight)
```

**单调性**：起点平衡时，前缀 0 本身就是一个「平衡且 `cum = 0`」的候选，任何被选中的前缀都不会比它差，因此 cut 跨 pass **单调不增**。

起点不平衡是唯一的例外：该趟可能为换取平衡而接受负的 `cum`，cut 反而上升一次。此后状态已平衡，单调性恢复。

### 5.4 增益更新规则

移动 `v: F → T` 时，对每条 `e ∋ v`：

```
// 移动前
if      cnt[e][T] == 0:  对所有未锁定的 u ∈ e:              gain[u] += nw[e]
else if cnt[e][T] == 1:  对 e 中唯一那个 part[u] == T 的 u，
                         若未锁定:                          gain[u] -= nw[e]

cnt[e][F] -= 1;  cnt[e][T] += 1

// 移动后
if      cnt[e][F] == 0:  对所有未锁定的 u ∈ e:              gain[u] -= nw[e]
else if cnt[e][F] == 1:  对 e 中唯一那个 part[u] == F 的 u，
                         若未锁定:                          gain[u] += nw[e]
```

每次 `gain[u]` 改变都要把 `u` 在桶间搬一次（O(1)）。

两条 `== 1` 分支里的「唯一那个 u」都必然不是 `v` 本身：前一条求值时 `v` 还在 `F` 侧，后一条求值时 `v` 已在 `T` 侧。实现时无需额外判断 `u != v`。

一趟 pass 的总代价是 O(pins)，这是 FM 的全部价值所在。

**固定点在这里的处理**：固定点 `locked` 恒为 true，因此上面四条分支都会跳过它们的 `gain` 更新（它们本来就不在桶里）。但它们**照常计入 `cnt[e]` 和 `wsum[]`**——这是本设计里最容易写错的一点，§6 有专门的测试。

---

## 6. 测试

`src/test_fmpart.cpp`，纯 `assert` + 失败返回非零 + 末尾打印汇总。仓库暂无测试框架，此为 CLAUDE.md 约定的形式。

### 6.1 第二个实例化

驱动内手写约 30 行的 `SimpleHypergraph`（`vector<vector<int>>` 存 pins，两个权重数组）。`FMPart<SimpleHypergraph>` 与 `FMPart<AbcNtkWrapper>` 同时编译通过，才算证明了模板化本身。

除真实电路测试外，所有算法测试都跑在 `SimpleHypergraph` 上：不依赖 ABC、快、易构造、可复现。

### 6.2 不变量检查

实现一个 `verify_invariants()`，在每趟 pass 结束后调用：

| 检查 | 做法 |
|---|---|
| cut 一致性 | 从 `part[]` 重算 cut，与增量跟踪值比对 |
| `cnt[]` 一致性 | 重算每条 net 两侧 pin 计数 |
| **`gain[]` 一致性** | 按 §4.3 定义重算全部增益，与增量维护值比对 |
| 桶结构一致性 | 每个未锁定顶点恰在其 `gain` 对应的桶内；`max_gain` 指针不虚高于实际最大值 |
| `wsum[]` 一致性 | 重算两侧权重和 |

`gain[]` 一致性是重点：§5.4 有四条分支，漏掉任何一条都不会崩溃，只会悄悄给出更差的 cut。没有这项检查，此类 bug 会长期潜伏。

### 6.3 功能测试

1. **已知最优**：手算最优 cut 的小超图，断言 FM 达到该值
2. **FixNode**
   - 两个顶点钉到两侧 → 断言全程未移动，且其贡献的 `cnt[e]` 正确
   - 一侧全部钉死 → 断言另一侧自由顶点仍能正常优化
   - 全部顶点钉死 → 断言 `cut` 等于该固定解的 cut，`passes` 正常终止
3. **平衡**
   - 断言最终 `wsum[0]`、`wsum[1]` 均 `<= max_weight`
   - 构造「仅固定点就已超重」的输入 → 断言 `balanced == false`，且不死循环、不崩溃
4. **加权**：非 1 的 `vertex_weight` 与 `net_weight` 各跑一组，确认平衡按顶点权重算、cut 按 net 权重算
5. **单调性**：记录每趟结束后的 cut。起点平衡时断言序列单调不增；起点不平衡时允许第一趟上升，断言其后单调不增（依据 §5.3 的单调性说明）
6. **退化输入**：空图、单顶点、无 net、所有 net 只含一个 pin —— 均不得崩溃

### 6.4 随机压测

固定种子生成若干随机超图（顶点数、net 数、pin 数随机），每个跑完整的 §6.2 + §6.3.1/3/5。种子写死在源码里，失败可复现。

### 6.5 真实电路

`AbcNtkWrapper` 读 `regression/SimpleCircuits/mcnc/*.v`，跑 2-way FM，与 `patoh <hgr> 2` 的 cut 并排打印。

**不设断言阈值**。FM 是单层 refinement，本就打不过多级的 patoh。这条测试的作用是确认建图口径正确、cut 量级合理，不作为回归门禁。

---

## 7. 已知重复：hpart 的建图逻辑

`src/hpart/hpart.cpp:164` 的 `BuildHypergraph()` 与本次的 `abc_wrapper.cpp` 将是近乎相同的逻辑（driver + 传递可达 sink）。

本次**不合并**。`hpart` 是可用的工作代码，在 `AbcNtkWrapper` 自身被验证之前改动它会把两件事的风险绑在一起。等 wrapper 经过 §6.5 验证，再单独提一次「让 `hpart` 改用 `AbcNtkWrapper`」的收敛更合适。

此重复在此明确记录，避免日后被当成无意的遗漏。

---

## 8. 构建集成

```
src/fmpart/CMakeLists.txt      新增 fmpart 库（abc_wrapper.cpp）
src/CMakeLists.txt             add_subdirectory(fmpart)
                               FoxSYN 链接 fmpart
                               新增 test_fmpart 可执行目标
```

`fmpart` 依赖 `libabc`（仅 `abc_wrapper.cpp` 需要）。`fmpart.hpp` 与 `fm_buckets.hpp` 不含 ABC 头文件，可脱离 ABC 单独使用。

验证：`make` 通过，`./release/test_fmpart` 返回 0。

---

## 9. 文件规模预估

| 文件 | 行数 |
|---|---|
| `src/fmpart/fm_buckets.hpp` | ~120 |
| `src/fmpart/fmpart.hpp` | ~350 |
| `src/fmpart/abc_wrapper.hpp` | ~50 |
| `src/fmpart/abc_wrapper.cpp` | ~120 |
| `src/test_fmpart.cpp` | ~250 |
