# agdmap Code Review

## Bugs (必须修)

### 1. AgdmapCommand.cpp:63 — map() 失败后继续调用 getNtk()
`mapper.map()` 返回非零时打印失败信息，但执行继续，无条件调用 `mapper.getNtk()`。
若 `agdmapToAbcLogic()` 未执行，`pNtk_` 仍是原始 AIG，非空指针通过后续 NULL 检查，
`Abc_FrameReplaceCurrentNetwork` 装入错误网络，静默破坏 ABC 会话。

**修法：** `map()` 非零时立即 `return 0`，不继续执行。

---

### 2. AgdmapCommand.cpp:77,84 — argv[++i] 越界：-k/-w 作为末尾参数时无边界检查
循环条件 `i < argc` 只保护下一轮迭代，不保护当前 `++i` 的下标访问。
用户执行 `agdmap -k`（无后续参数）→ `++i == argc` → `argv[argc]` 是 UB，通常为
nullptr → `atoi(nullptr)` 崩溃；或静默传入 0 绕过 `k < 3` 校验，触发 `cutEnum` 内的
`assert(0)`。

**修法：** 取参数前加 `if ((size_t)(i + 1) >= argc) goto usage;`。

---

### 3. utility.h:378 — Petrick 法中 `1 << i` 有符号溢出（i >= 31）
`getUsedVars` 的 Petrick 求解器用 `std::vector<size_t>` 存位掩码，但填充时写
`1 << i`（signed int）。函数有 >= 32 个质蕴含项时（多变量稀疏函数可达到），
`i >= 31` → 有符号左移 UB → M0/M1 中写入错误位掩码 → 变量使用标记错误 →
cut 函数表达式出错，后续 truth table 求值生成错误逻辑，无任何报错。

**修法：** 改为 `size_t(1) << i` 或 `1ull << i`。

---

### 4. utility.h:925 — ordered_merge reserve 拼写错误
```cpp
result.reserve(vec_a.size() + vec_a.size()); // 应为 + vec_b.size()
```
当 `vec_b` 远大于 `vec_a` 时，预留容量只有需要量的一半，
`Decompose::combine()` 内循环中触发反复 reallocation，影响 SimpleGate 大规模合并性能。

**修法：** 改为 `result.reserve(vec_a.size() + vec_b.size())`。

---

### 5. agdmap.cpp:265 — assert(type > 0) 与枚举定义矛盾
`agdmap.h` 第 89 行：`Lut = 0` 是 `sol_type` 的唯一值。
`NodeSol::getLevel()` 在 `ll == 0` 时执行 `assert(type > 0)`，即对所有
正常 LUT 节点初次调用 `getLevel()` 时触发 assert → debug 构建直接 abort。

**修法：** 删除该 assert，或将其改为 `assert(type == Lut)`（确认预期分支正确后删除）。

---

### 6. agdmap.cpp:1190 — const0/const1 节点未命名，Abc_NtkFindNode 返回 nullptr
内部节点循环（line 1102-1113）对常量函数 LUT 创建 ABC 节点时不调用
`Abc_ObjAssignName`。若该节点 `weight > 1`（多个 PO 共享此 AND），
PO 循环在 line 1190 按名称查找该节点 → 返回 nullptr →
`Abc_ObjAddFanin(pNet, nullptr)` 崩溃。

**修法：** const0/const1 节点也需要调用 `Abc_ObjAssignName`，
或在 weight > 1 路径前加空指针检查。

---

## 性能优化

### 7. agdmap.h:427 + agdmap.cpp:807 — std::map 热路径 + 每次 cutSel 整图拷贝
`required_`、`sol_`、`weight_` 使用 `std::map<Node*, ...>`，cutSel 每次调用
`auto required = required_` 触发完整 O(n) 深拷贝（每个节点一次 red-black tree
节点分配）。cutSel 被 itrSel 最多调用 8 次，area+flow 两轮共 32 次拷贝。
每次访问 map 是 O(log n)，热路径上节点 ID 连续，换成 flat vector 可变为 O(1)。

**修法：**
```cpp
// 替换：
std::map<Node*, Level> required_;
std::map<Node*, pCut>  sol_;
std::map<Node*, int>   weight_;

// 为：
std::vector<Level> required_;   // indexed by node->getId()
std::vector<pCut>  sol_;
std::vector<int>   weight_;
```
cutSel 中 `auto required = required_` 改为 `const auto& required = required_`，
需要修改时原地操作。

---

### 8. Function 用字符串表示 truth table — 最大算法级瓶颈
`truth_table` 类用字符串（如 `"(n1)*((n2)*(n3))"`）存函数，`mergeCut` 每次合并
做字符串拼接。`agdmapToAbcLogic` 对每个节点跑 Quine-McCluskey + Petrick 法求真值表，
Petrick 在质蕴含项多时是指数级。

**修法：** 改用 `uint64_t` 直接存 6 变量 truth table（标准做法）：
- `mergeCut` 里 AND 变成 `left_tt & right_tt`，NOT 变成 `~tt`，均为单条指令
- `agdmapToAbcLogic` 直接用 truth table 调用 `Abc_SopFromTruthHex`，无需 QMP

---

### 9. agdmap.cpp:723,732 — itr_num=8 硬编码两处，不自适应电路规模
两处独立的 `int itr_num = 8` 可以各自被修改而产生不一致。
小电路 8 轮全是浪费；大电路可能收敛需要更多轮。

**修法：** 加入 `Para`：`para_.itr_num`，或根据节点数自适应，
如 `std::min(8, std::max(3, (int)std::log2(nodes_.size())))`。

---

## 代码质量

### 10. agdmap.cpp:59 + 1597 — area/delay 两分支内层循环体完全重复
`Node::cutEnum`（line 79-87 vs 109-117）和 `SimpleGate::combine`
（line 1600-1626 vs 1630-1653）的 mergeCut/pruner 循环体在两个分支中一字不差。
修复 ordered_merge reserve bug（见第 4 条）或调整 pruner 策略时，
必须同时改两处，漏改其中一处会使 area 模式和 delay 模式产生不同行为。

**修法：** 提取共用 lambda 或私有函数，分支只传入不同的 pruner 类型和参数。
