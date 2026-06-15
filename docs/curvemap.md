# curvemap — Area-Delay Pareto Curve LUT Mapper

## 背景

将 `src/curvemap/map.{h,cc}` 中基于 DataModel 的 LUT mapping 算法核心（cut 枚举 + area/delay Pareto 曲线 + cut 选择）移植到 ABC 的 strash AIG (`Abc_Ntk_t`)，彻底脱离 DataModel/Pin/Net/nimbus。注册为 ABC 命令 `curvemap`，模型采用 unit delay（每个 LUT 延迟为 1，无边延迟）。

## 对照原算法的取舍

| 原算法特性 | 移植决策 |
|---|---|
| `nextCut` 混合进制多 fanin 合并 | 删。AIG 每个 AND 只有 2 fanin，退化为双层循环 |
| MUXF7/MUXF8（`_muxf7_cost`、`_muxf8_cost`、器件约束） | 删 |
| STA-based delay（CR#19437 已关掉） | 删。只用 unit delay |
| 原 `lutGen`（DataModel collapse） | 替换为 `Abc_SopCreateFromTruth` + 递归建 mapped LUT `Abc_Ntk_t`，参照 supermap |
| `indexCmp`（靠 `setIndex` 拓扑序 + pin 指针排序） | 用 `Abc_ObjId` 天然整数升序，cut leaves 存 `std::vector<int>` |
| `Pin*` → cut 的映射 | `std::vector<std::vector<Cut*>>`，按 `Abc_ObjId` 索引，容量 `Abc_NtkObjNumMax` |
| Shannon 分解（K+1 cut 兜底） | 删。严格保证 cut ≤ K |
| Optimization target 选择 | 沿用原 non-STA 单一模式：先求全局 min delay（`max_arrival`），再 delay-bounded 面积恢复 |

## 核心数据结构

### CutSolution — 曲线上的一个点

```cpp
struct CutSolution {
    double area;   // area-flow 面积
    int    delay;  // 延迟（unit delay 下 = LUT 层数）
    int    level;  // 逻辑级数（unit delay 下 = delay）
};
```

支配关系 (`compare`): 三条判据 `(area, delay, level)` 同时使用：
- `DOM_L`: 三方都不差于 rhs，至少一方严格优 → 左支配右
- `DOM_R`: 右支配左
- `PRE/SUCC`: 互不支配，按 area 排先后
- `SAME`: 全等
- `delta` 容差用于浮点 area 比较（默认 0.0）

### CutCost — area-delay Pareto 前沿

```cpp
class CutCost {
    std::vector<CutSolution> _curve;  // 按 area 递增排序
};
```

- `insert(CutSolution)`: 按 area 插入正确位置，扫描剔除被支配解
- `prune(int nsol)`: 解数 > nsol 时等步长抽稀，保留首尾
- `getSolForDelay(int target)`: 从头部（min area）开始，返回第一个 `delay ≤ target` 的解；若都不满足返回 `minDelaySol()`（尾部）
- `minAreaSol()` = `_curve.front()`, `minDelaySol()` = `_curve.back()`
- 常量 `SOL_BOUND = 10`

### Cut — 一个 K-feasible cut

```cpp
class Cut {
    std::vector<int> _leaves;     // 叶子 ObjId，升序
    CutCost          _cost;       // 普通 LUT 解曲线
    uint64_t         _truth;      // 真值表（K ≤ 6，单 uint64）
    bool             _is_trivial; // 是否为平凡 cut
};
```

- 所有 cut 都是 "full cut"（不再区分 base / non-base，MUXF 已删）
- Trivial cut: `_leaves = {node_itself}`，代表该节点作为一个 LUT 输入信号时的代价视图（它会在合并时被父 cut 扣掉自身 LUT 成本）

### Curvemap — 映射器主类

```cpp
class Curvemap {
    Abc_Ntk_t* _pNtk;
    int _K;               // LUT 输入数
    int _nObjs;           // = Abc_NtkObjNumMax(pNtk)

    std::vector<std::vector<Cut*>> _cuts;     // [objId] → cut list
    std::vector<int>               _level;     // [objId] → logic level
    std::vector<int>               _required;  // [objId] → required time
    std::vector<bool>              _is_root;   // [objId] → is LUT root
    std::vector<Cut*>              _best_cut;  // [objId] → selected cut

    std::vector<Abc_Obj_t*> _topo_order;  // 拓扑序所有节点
    std::vector<int>        _pi_ids;      // PI ObjId 列表
    std::vector<int>        _po_ids;      // PO ObjId 列表
    int _max_arrival;                     // 全局 min delay
};
```

## 参数常量（unit delay model）

```cpp
static constexpr double LUT_AREA  = 1.0;
static constexpr int    LUT_DELAY = 1;
static constexpr int    EDGE_DELAY = 0;
static constexpr int    SOL_BOUND  = 10;
static constexpr int    CUT_LIMIT  = 30;
```

## 算法主流程: `Curvemap::run()`

```
1. 拓扑排序 → _topo_order (Abc_NtkDfs)
2. 分配所有 vector（size = _nObjs）
3. Forward pass: cut_enum()
   - 遍历 _topo_order，对每个节点做 cutEnu
   - PI: init_pi_cuts()
   - AND node: cut_enum_node()
   - PO driver 处累积 _max_arrival
4. Backward pass: cut_select()
   - 逆拓扑序处理
   - PO driver: required = _max_arrival, mark root
   - Root 节点: cutSel → _best_cut, 传播 required / root 到 cut leaves
5. Build mapped network: build_mapped_ntk()
   - 递归从 PO 往回建 LUT 节点，真值表 → SOP
6. 打印 QoR
```

## Forward Pass 详解

### PI trivial cut

```cpp
void init_pi_cuts(int piId) {
    Cut* cut = new Cut;  // trivial, leaves = [piId]
    cut->_leaves.push_back(piId);
    cut->_is_trivial = true;
    // PI 真值表 = var0 = 0xAAAA...（在合并时用 supermap 的变量初始化方式）
    cut->_truth = 0xAAAAAAAAAAAAAAAAULL;
    // PI: arrival = 0, level = 0
    // 平凡 cut 解: area = LUT_AREA, delay = arrival + LUT_DELAY + EDGE_DELAY = 0+1+0 = 1, level = 0+1 = 1
    cut->_cost.insert(CutSolution(LUT_AREA, 1, 1));
    _cuts[piId].push_back(cut);
}
```

### AND node cut enumeration

```cpp
void cut_enum_node(Abc_Obj_t* pNode) {
    int nId = Abc_ObjId(pNode);
    int f0 = Abc_ObjFaninId(pNode, 0);  bool c0 = Abc_ObjFaninC(pNode, 0);
    int f1 = Abc_ObjFaninId(pNode, 1);  bool c1 = Abc_ObjFaninC(pNode, 1);
    int fo0 = Abc_ObjFanoutNum(Abc_ObjFanin(pNode, 0));
    int fo1 = Abc_ObjFanoutNum(Abc_ObjFanin(pNode, 1));

    for (Cut* cut0 : _cuts[f0]) {
        for (Cut* cut1 : _cuts[f1]) {
            // 1. Merge leaves
            std::vector<int> merged_leaves;
            std::set_union(cut0->_leaves.begin(), cut0->_leaves.end(),
                           cut1->_leaves.begin(), cut1->_leaves.end(),
                           std::back_inserter(merged_leaves));
            if (merged_leaves.size() > _K) continue;

            // 2. Redundancy check
            if (is_redundant(nId, merged_leaves)) continue;

            // 3. Compute truth table
            uint64_t tt = compute_truth(merged_leaves, cut0, cut1, c0, c1);

            // 4. Create merged cut with cost curve
            Cut* merged = new Cut;
            merged->_leaves = std::move(merged_leaves);
            merged->_truth = tt;

            // Seed: this LUT's own cost
            merged->_cost.insert(CutSolution(LUT_AREA, LUT_DELAY, 1));

            // Combine costs from both fanins
            combine_cost(merged, cut0, fo0);
            combine_cost(merged, cut1, fo1);

            merged->_cost.prune(2 * SOL_BOUND);  // 中间 prune 到 20

            // 5. Insert into node's cut list
            insert_cut(nId, merged);
        }
    }

    // 6. Create trivial cut for this node
    create_trivial_cut(nId);
}
```

### Area-flow cost combination

```cpp
// 将一个 fanin 的 cut 解曲线合并进 merged cut
void combine_cost(Cut* merged, Cut* subcut, int fanout) {
    CutCost newcost;
    for (auto& sol : merged->_cost) {
        for (auto& sub_sol : subcut->_cost) {
            CutSolution nsol = sol;
            // area: 减去 subcut 自身 LUT 面积后按 fanout 分摊
            nsol.incArea((sub_sol.area - LUT_AREA) / double(fanout));
            // delay: sub_sol.delay - sub_LUT_DELAY + this_LUT_DELAY = sub_sol.delay
            nsol.updateDelay(sub_sol.delay);
            nsol.updateLevel(sub_sol.level);
            newcost.insert(nsol);
        }
    }
    merged->_cost = std::move(newcost);
}
```

### Trivial cut 生成

```cpp
void create_trivial_cut(int nId) {
    Cut* triv = new Cut;
    triv->_leaves.push_back(nId);
    triv->_is_trivial = true;
    triv->_truth = 0xAAAAAAAAAAAAAAAAULL;

    if (_cuts[nId].empty()) {
        // PI-like: arrival = _level[nId]
        triv->_cost.insert(CutSolution(LUT_AREA, _level[nId] + LUT_DELAY, _level[nId] + 1));
    } else {
        // 从已有各 cut 的解"抬升一层"
        for (auto& c : _cuts[nId]) {
            for (auto& sol : c->_cost) {
                CutSolution nsol = sol;
                nsol.incArea(LUT_AREA);
                nsol.incDelay(LUT_DELAY);
                nsol.incLevel();
                triv->_cost.insert(nsol);
            }
        }
        // Note: 原代码对 CARRY4/CARRY8 inst pin 硬编码 area=2 的 FIXME 删除
    }
    triv->_cost.prune(SOL_BOUND);
    _cuts[nId].insert(_cuts[nId].begin(), triv);  // push_front（trivial cut 靠前可能影响 insert 排序逻辑？原代码 pushFrontCut）
}
```

### Cut 插入与去重

```cpp
bool insert_cut(int nId, Cut* cut) {
    auto& cutlist = _cuts[nId];
    bool near_full = (cutlist.size() > 2 * CUT_LIMIT / 3);

    for (auto it = cutlist.begin(); it != cutlist.end(); ) {
        Cut* old = *it;
        if (cut->leaves_contain(old)) {  // cut is redundant (superset)
            delete cut; return false;
        }
        if (old->leaves_contain(cut)) {  // old is redundant
            it = cutlist.erase(it); delete old; continue;
        }
        // Cost curve dominance
        CutCost::Rel rel = cut->compare_cost(old);
        switch (rel) {
        case CutCost::SAME:
            if (cut->size() < old->size())
                { it = cutlist.erase(it); delete old; continue; }
            else
                { delete cut; return false; }
        case CutCost::DOM_L:
            if (cut->size() <= old->size() || near_full)
                { it = cutlist.erase(it); delete old; continue; }
            break;
        case CutCost::DOM_R:
            if (cut->size() >= old->size() || near_full)
                { delete cut; return false; }
            break;
        }
        ++it;
    }
    if (cutlist.size() < CUT_LIMIT) {
        cutlist.push_back(cut);
        return true;
    }
    delete cut;
    return false;
}
```

## 真值表计算

完全照搬 supermap `Cut::compute_truth`（`src/supper/cut.cpp:49-81`）：

```cpp
uint64_t compute_truth(const std::vector<int>& merged_leaves,
                       const Cut* c0, const Cut* c1,
                       bool compl0, bool compl1) {
    uint64_t tt0 = c0->_truth;
    uint64_t tt1 = c1->_truth;

    // Permute each fanin's truth table to match merged cut's variable order
    auto permute = [&](uint64_t& tt, const Cut* sub) {
        int k = (int)sub->size() - 1;
        for (int i = (int)merged_leaves.size() - 1; i >= 0 && k >= 0; i--) {
            if (merged_leaves[i] > sub->_leaves[k]) continue;
            if (k < i)
                Abc_TtSwapVars(&tt, (int)merged_leaves.size(), k, i);
            k--;
        }
    };
    permute(tt0, c0);
    permute(tt1, c1);

    if (compl0) tt0 = ~tt0;
    if (compl1) tt1 = ~tt1;

    return tt0 & tt1;
}
```

`Abc_TtSwapVars` 来自 `misc/util/utilTruth.h`，对 K ≤ 6 自动落到 `Abc_Tt6SwapVars`。

## Backward Pass 详解

```cpp
void cut_select() {
    // 逆拓扑序
    for (int i = (int)_topo_order.size() - 1; i >= 0; i--) {
        Abc_Obj_t* pObj = _topo_order[i];
        int nId = Abc_ObjId(pObj);

        if (Abc_ObjIsCo(pObj)) {
            // PO: 设 required = _max_arrival，标记 driver 为 root
            int drvId = Abc_ObjFaninId(pObj, 0);
            _required[drvId] = _max_arrival;
            _is_root[drvId] = true;
            continue;
        }

        if (!_is_root[nId]) continue;

        // PI root: skip (no cut selection needed)
        if (Abc_ObjIsCi(pObj)) continue;

        // Select best cut
        Cut* best = cut_sel(nId);
        _best_cut[nId] = best;

        // Propagate to cut leaves
        for (int leaf : best->_leaves) {
            int leaf_req = _required[nId] - LUT_DELAY;
            _required[leaf] = std::max(_required[leaf], leaf_req);
            _is_root[leaf] = true;
        }
    }
}

Cut* cut_sel(int nId) {
    int req = _required[nId];
    Cut* best = nullptr;
    CutSolution best_sol;

    for (Cut* c : _cuts[nId]) {
        if (c->_is_trivial) continue;
        CutSolution& sol = c->_cost.getSolForDelay(req);
        if (sol.delay > req) continue;
        if (!best ||
            sol.area < best_sol.area ||
            (sol.area == best_sol.area && sol.delay < best_sol.delay)) {
            best = c;
            best_sol = sol;
        }
    }
    if (!best) {
        // Fallback: use min delay cut
        fprintf(stderr, "curvemap warning: no cut meets required at node %d\n", nId);
        for (Cut* c : _cuts[nId]) {
            if (c->_is_trivial) continue;
            if (!best || c->_cost.minDelaySol().delay < best_sol.delay) {
                best = c;
                best_sol = c->_cost.minDelaySol();
            }
        }
    }
    return best;
}
```

## Mapped LUT Network 构建

完全参照 supermap `create_abc_ntk_from_mapping`（`src/supper/map.cpp:374-416`）和 `create_lut_obj_rec`（`src/supper/map.cpp:322-372`）。

### 主函数

```cpp
Abc_Ntk_t* build_mapped_ntk() {
    Abc_Ntk_t* ntk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);

    // PI
    std::vector<Abc_Obj_t*> cache(_nObjs, nullptr);
    for (Abc_Obj_t* pi : _pi_nodes) {
        int piId = Abc_ObjId(pi);
        Abc_Obj_t* newPi = Abc_NtkCreatePi(ntk);
        Abc_ObjAssignName(newPi, Abc_ObjName(pi), nullptr);
        cache[piId] = newPi;
    }

    // Const-1
    Abc_Obj_t* const1 = Abc_NtkCreateNodeConst1(ntk);
    cache[Abc_ObjId(Abc_AigConst1(_pNtk))] = const1;

    // PO
    for (int poId : _po_ids) {
        Abc_Obj_t* pPo = Abc_NtkObj(_pNtk, poId);
        Abc_Obj_t* newPo = Abc_NtkCreatePo(ntk);
        Abc_ObjAssignName(newPo, Abc_ObjName(pPo), nullptr);

        int drvId = Abc_ObjFaninId(pPo, 0);
        bool drvCompl = Abc_ObjFaninC(pPo, 0);

        Abc_Obj_t* lut = create_lut_rec(ntk, drvId, cache);
        if (drvCompl)
            lut = Abc_NtkCreateNodeInv(ntk, lut);
        Abc_ObjAddFanin(newPo, lut);
    }

    // Cleanup unused const1
    if (Abc_ObjFanoutNum(const1) == 0)
        Abc_NtkDeleteObj(const1);

    // Post-process
    Abc_NtkLogicMakeSimpleCos(ntk, 0);
    Abc_NtkCheck(ntk);

    return ntk;
}
```

### 递归 LUT 构建

```cpp
Abc_Obj_t* create_lut_rec(Abc_Ntk_t* ntk, int nId,
                           std::vector<Abc_Obj_t*>& cache) {
    if (cache[nId]) return cache[nId];  // memoized

    Abc_Obj_t* pLut = Abc_NtkCreateObj(ntk, ABC_OBJ_NODE);
    cache[nId] = pLut;

    Cut* best = _best_cut[nId];
    uint64_t tt = best->_truth;

    if (tt == 0ULL || tt == ~0ULL) {
        // Degenerate: constant 0 or 1
        Abc_ObjAddFanin(pLut, cache[Abc_ObjId(Abc_AigConst1(_pNtk))]);
        pLut->pData = (tt == 0ULL)
            ? Abc_SopCreateBuf((Mem_Flex_t*)ntk->pManFunc)
            : Abc_SopCreateInv((Mem_Flex_t*)ntk->pManFunc);
    } else {
        pLut->pData = Abc_SopRegister((Mem_Flex_t*)ntk->pManFunc,
            Abc_SopCreateFromTruth((Mem_Flex_t*)ntk->pManFunc,
                (int)best->size(), (unsigned*)&tt));

        // Recursively create fanin LUTs
        for (int leaf : best->_leaves) {
            Abc_Obj_t* pFanin;
            if (_is_root[leaf] && _best_cut[leaf]) {
                pFanin = create_lut_rec(ntk, leaf, cache);
            } else {
                // PI or already cached
                pFanin = cache[leaf];
                assert(pFanin);
            }
            Abc_ObjAddFanin(pLut, pFanin);
        }
    }
    return pLut;
}
```

注意：由于 backward pass 中所有 cut leaf 都被 mark 为 root 且会在逆拓扑序中被 `cutSel` 选择，`_best_cut[leaf]` 必然存在（除非 leaf 是 PI/const1，此时 `cache[leaf]` 已填）。因此 `create_lut_rec` 不需要 fallback 到 "not root" 分支。

## 工程布局

完全对齐 agdmap 的 vendoring 模式：

```
src/curvemap/
├── CMakeLists.txt           # add_library(curvemap curvemap.cpp CurvemapCommand.cpp)
├── curvemap.h               # namespace fox::curvemap，核心类定义
├── curvemap.cpp             # 核心实现（cut enum / select / build）
└── CurvemapCommand.cpp      # extern "C" int Curvemap(...)，命令行参数解析
```

需修改的外部文件：

1. **`src/CMakeLists.txt`**: 加 `add_subdirectory(curvemap)` 和 `target_link_libraries(FoxSYN PRIVATE ... curvemap)`
2. **`src/main.cpp`**: 加 `#include "curvemap/CurvemapCommand.h"` 和 `Cmd_CommandAdd(..., "curvemap", Curvemap, 1)`

命令用法：`curvemap [-K <lut_size>]`，默认 K=6。

## 合并时的面积-延迟语义（再确认）

结合 `combineSol`（原 `map.cc:200-218`）和 trivial cut 生成（原 `map.cc:590-647`），以 unit-delay 参数代入后的语义如下：

**非平凡 cut（AND 节点的 merged cut）**: 表示以该 AND 节点为 root 的一个 LUT，其 cost 包括：
- `1.0`（该 LUT 自身的面积）+ `1`（该 LUT 自身的延迟）
- 加上各 fanin 的子图代价（去除子 LUT 面积后按 fanout 分摊）

延迟为 `max(自身延迟, 各 fanin 贡献延迟)`。由于自身延迟 = 1，fanin 延迟在 unit 模型下 ≥ 0，所以最小组延迟 ≥ 1。

**Trivial cut**: 同一节点作为"信号"（准备作为上级 LUT 输入）时的代价。在非平凡解基础上再 +1 area / +1 delay / +1 level。当父节点组合时，公式 `(sub_sol.area - 1.0) / fanout` 恰好把 trivial cut 多带的那 1.0 area 扣回去，只留子图面积参与分摊。

这种设计保证了 PI 的 trivial cut（area=1.0, delay=1）在被 parent 组合时贡献为 0 area（PI 不消耗 LUT），延迟恰好 = 1（被 parent 的 own delay=1 吸收）。

## 已知问题 / TODO

1. 当前暂不支持 K > 6（真值表仅 uint64），如需支持需扩展 multi-word truth table 或使用 ABC 的 hash table 存储
2. 真值表变量初始化（PI trivial cut 的 `0xAAAA...`）需要与 `compute_truth` 的变量置换逻辑匹配。PI 的初值含义是"变量 0 = 信号自身"，当 PI 在 merged cut 中被置换到正确位置后，真值表正确反映 AND 功能
3. 未实现 timing-driven（STA-based）模式，与原算法一样
4. `getCutArea`/`getCutDelay` 对 inst/preserve 节点的特殊逻辑在 ABC AIG 中天然不存在（AIG 全由 AND + PI 组成），相关分支删除
