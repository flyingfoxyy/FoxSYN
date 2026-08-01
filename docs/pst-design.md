# pst — 分区感知的 structural hashing 设计

## 背景与动机

`st`（`Abc_NtkStrash`，`src/abc/src/base/abci/abcStrash.c:265`）把 LUT netlist 打散成 AIG，靠全局结构哈希表复用等价的 AND 节点。这个复用是纯功能性的：只要两个 AND 的 `(fanin0, c0, fanin1, c1)` 相同就合并成一个节点。

在已经分好区的网表上，这个行为会摧毁分区边界。设想 LUT A 在分区 0、LUT B 在分区 1，两者内部都展开出 `a & b` 这个子结构。`st` 会把它们合并成一个 AND 节点，而这个节点只能属于一个分区——另一个分区从此少了一块本该属于自己的逻辑，跨分区边随之凭空出现。分区信息在 `st` 之后基本不可用。

`pst` 做的是**分区感知的结构哈希**：只有 `part_id` 相同的 AND 才允许复用。同分区的共享照常发生（包括跨 LUT 的共享），跨分区的合并被拦下来，代价是多建一个重复节点。输出是真正的 `ABC_NTK_STRASH` 网络，每个 AND 都带 `part_id`。

`pst` 只负责 LUT netlist → AIG 这一步。分区感知的 mapping 是独立的后续工作，不在本设计范围内（详见"已知限制"）。

## 命令接口

```
pst
```

无参数。前置检查三条，照 `pdecomp.cpp:137-152` 的风格：

- `pNtk` 非空
- `Abc_NtkIsLogic(pNtk)` 为真（不能已经是 AIG）
- `pNtk->pPdb` 非空（必须先跑 `hpart`）

## 架构

### 构造新网络，而非原地修改

`pst` 跟 `st` 一样构造一个新的 `ABC_NTK_STRASH` 网络，成功后用 `Abc_FrameReplaceCurrentNetwork` 装进 frame。这跟 `pdecomp` 的原地修改 + `Abc_NtkDup` 快照回滚不同——因为新网络是独立对象，失败时 `Abc_NtkDelete` 掉它就行，frame 里的原 LUT netlist 从头到尾没被碰过，回滚是天然的。

### 拷贝 ABC 代码，不修改 ABC

所有从 ABC 拷进 `src/pst/pst.cpp` 的代码：

| 来源 | 拷贝内容 | 改动 |
|---|---|---|
| `abcStrash.c:265` `Abc_NtkStrash` | 顶层流程 | 加不变量检查，去掉 EXDC/record 分支 |
| `abcStrash.c:413` `Abc_NtkStrashPerform` | DFS 遍历 | 传递宿主 LUT 的 `part_id` |
| `abcStrash.c:468` `Abc_NodeStrash` | 单节点 strash | 同上 |
| `abcStrash.c:445` `Abc_NodeStrash_rec` | Hop DAG 递归 | `Abc_AigAnd` → `pst_and` |
| `abcAig.c:52` `struct Abc_Aig_t_` | 结构体布局镜像 | 只读 `pBins`/`nBins`/`nEntries` |
| `abcAig.c:90` `Abc_HashKey2` | 哈希函数 | 无 |
| `abcAig.c:319` `Abc_AigAndCreate` | 强制建重复点 | 去掉 resize 检查（见下） |

**为什么必须镜像私有结构体**：`struct Abc_Aig_t_` 只定义在 `abcAig.c:52`，`abc.h:118` 仅有前向声明；`Abc_AigAndCreate` 是 `static`。要在 STRASH 网络里造出"同 fanin 不同分区"的第二个 AND，必须绕过 `Abc_AigAndLookup` 直接往 `pBins` 里插，而访问 `pBins` 就得知道结构体布局。

**这是本设计唯一的脆弱点。** ABC 是独立子仓库、可能从上游更新，如果 `Abc_Aig_t_` 增删字段，镜像会静默失配（读到错误偏移）。缓解措施：在镜像定义处注释标注源文件与行号、字段必须逐字对应，并在 `pst.cpp` 顶部集中说明这个耦合。

### 为什么选"插入真实哈希表"而非其他方案

考虑过三种，选了插入真实表：

**方案 A（采纳）**：查找走 pst 自己的分区感知 map，插入走 ABC 的真实 `pBins`（普通节点用公开 `Abc_AigAnd`，重复节点用拷贝来的强制建点）。每个节点都在 ABC 的表里，所以 `Abc_AigCheck` 的硬检查 `Counter == Abc_NtkNodeNum`（`abcAig.c:263`）通过，`Abc_NtkCheck` 整体通过；`Abc_AigCleanup` 能看到所有节点。被遮蔽的重复节点会触发 `abcAig.c:256` 的 `not in the structural hashing table` 提示，但那条是 `printf`、不 `return 0`，属预期噪声。

**方案 B（否决）**：只用 pst 自己的 map，节点用 `Abc_NtkCreateNode` 手工造、完全不进 ABC 的表。代码最干净、不碰私有结构，但 `Abc_AigCheck` 会在 `Counter != NodeNum` 上**硬失败返回 0**，`Abc_NtkCheck` 跟着失败；`Abc_AigCleanup` 看不见这些节点。得到一个自称 STRASH 但表是空的网络，下游行为未知。

**方案 C（否决）**：插入真实表但哈希键含 `part_id`。从 pst 视角表自洽，但 ABC 自己的 `Abc_AigAndLookup` 用不含 part 的键去搜，几乎每个节点都查不到，`AigCheck` 对几乎所有节点报警告，且后续 ABC 的 `Abc_AigAnd` 会大量重复建点。相比 A 无任何收益。

## 核心算法

### 分区感知的 AND 构造

`pst_and(pMan, p0, p1, part)` 替代 `Abc_AigAnd`：

```
normalize: 若 p0.Id > p1.Id 则交换           # 跟 AigAndLookup/AigAndCreate 的规范化一致
key = (p0.Id, c0, p1.Id, c1, part)

if key 在 pst 的 unordered_map 里:
    return 命中节点                          # 同分区复用

if Abc_AigAndLookup(pMan, p0, p1) == NULL:
    pAnd = Abc_AigAnd(pMan, p0, p1)         # ABC 表里没有 -> 走公开 API
else:
    pAnd = pst_force_create(pMan, p0, p1)   # 已有别分区的同 fanin 节点 -> 强制建重复点
    dup_blocked++

Abc_ObjSetPartId(pAnd, part)
map[key] = pAnd
return pAnd
```

分两条路是有意的：**绝大多数节点走公开的 `Abc_AigAnd`**，表的 resize 由 ABC 自己处理（`abcAig.c:324`），所以 `pst_force_create` 不需要拷 resize 逻辑。重复点是少数，偶尔跳过一次 resize 检查只会让 bin 链表略长，不影响正确性。

`Abc_AigAndLookup` 的平凡情况（`p0 == p1`、`p0 == !p1`、任一侧是常量，`abcAig.c:410-425`）返回的不是新节点而是既有对象或常量，此时不进 map、不打 `part_id`，直接返回——这些是 AIG 层面的化简，没有对应的物理节点。

### part_id 的来源与归属

`part` 在 `pst_node_strash(pNtkNew, pNodeOld)` 里取一次：`part = Abc_ObjGetPartId(pNodeOld)`，整个 Hop 锥的递归共用这一个值。

**同一个 LUT 内部的所有 AND 同分区，所以 LUT 内共享完全不受影响；只有跨 LUT 且跨分区的合并被拦住。** map 是全局一张、跨所有 LUT，所以同分区的跨 LUT 共享照常发生——这正是我们想保留的复用。

各类对象的 `part_id`：

- **PI**：`Abc_NtkStartFrom` → `Abc_NtkDupObj` 已经逐个拷贝（`abcObj.c:399`），不用管
- **AND**：按上面赋值
- **const1**：留 `ABC_PART_ID_NONE`。AIG 里常量是全局唯一的 `Abc_AigConst1`，无法按分区分身。留 `NONE` 后它在所有指标计算里被跳过（`abcPdb.cpp:65` 的 `!Abc_ObjHasPartId` continue），是安全选择
- **PO**：`Abc_ObjIsPartStatVertex`（`abcPdb.cpp:36`）不把 PO 算作统计顶点，无需处理

`balance_pct` 需要手工搬一次：`pNtkAig->pPdb->set_balance_pct(pNtk->pPdb->balance_pct())`。ABC 层没有暴露这个字段的 C API，直接 include `abcPdb.hpp` 访问（`pdecomp.cpp:4` 有先例）。

### 节点删除的正确性

`Abc_AigAndDelete`（`abcAig.c:556-566`）在 bin 链表里**按指针身份**匹配（`if (pAnd != pThis) continue`），不是按 key 匹配。所以同 key 的多个重复项共存时，后续 `Abc_AigCleanup` 删悬空节点能正确删掉目标那一个，不会误删同 key 的兄弟。

## 不变量与失败处理

### 硬断言：hop + cut-net

```
pst: strashed to AIG (hop=3, cut-net=13)
pst: cut-edge 42 -> 41, cut-edge2 400 -> 1153
pst: nodes 122 -> 1153, dup-blocked 87
```

违反时打印并整体放弃：

```
pst: partition invariant violated (hop 3->4, cut-net 13->15), aborting
```

`Abc_NtkDelete` 新网络，frame 保持原 LUT netlist，返回 false。

**hop**（`Abc_NtkComputeHopNum`，`abcPdb.cpp:249`）只在 fanin 跨分区时 +1，与逻辑层数无关，所以 LUT 展开成多层 AND 不影响它。**cut-net**（`Abc_NtkComputeCutSize`）判断一个网是否被切，取决于消费者所在分区集合，展开不改变这个集合。两者理论上必须严格相等，因此作为实现正确性的断言。

### 两个 cut-edge 只报告，不断言

**`cut-edge2`（raw pair 数，`Abc_NtkComputeCutEdgeNum`，`abcPdb.cpp:181`）必然大幅上升。** AIG 表达不了 1-input 节点（`Abc_AigCheck:244` 拒绝单输入节点，`AND(a,a)` 在 lookup 里塌陷），所以 `pdecomp` 用的那种跨分区恒等缓冲（`pdecomp.cpp:117-127`）在 `pst` 里无法复制。一个跨分区 fanin 被展开锥内多个 AND 引用时，原来 1 条 `LUT→a` 边变成多条 `AND→a`。这是设计后果，不是缺陷。

**`cut-edge`（dedup，`Abc_NtkComputeCutEdgeDedupNum`，`abcPdb.cpp:211`）理论上不变但可能合法下降。** 它按驱动点统计不同目的分区数，展开不改变这个集合——除了一种情况：vacuous fanin（在 LUT 的 fanin 列表里但 Hop DAG 里没被引用）在 AIG 里彻底消失，它的边跟着消失。此外 `st` 路径上的常量折叠、Hop 化简也可能让某个 fanin 不再被引用。这些都是合法的下降，所以不作断言。

不把 dedup cut-edge 作硬断言是刻意的：它的不变性依赖"每个非 vacuous fanin 在 Hop DAG 里至少被引用一次"这个假设，一旦某条化简路径破坏了假设，断言会误报失败。宁可漏检也不误报。

**打印标签的对齐**：`ps` 把 dedup 数印成 `cut-edge`、raw 数印成 `cut-edge2`（`abcPrint.c:414-415`），而 `pdecomp` 自己的消息里 `cut-edge` 指的是 raw 那个。`pst` 的输出跟 `ps` 对齐，避免同一个词在两个命令里指不同东西。

### dup-blocked 诊断

唯一的额外诊断计数：因分区差异被拦下、因此多建的节点数。一行输出，直接量化"分区感知让 AIG 大了多少"。不做逐节点日志——`pdecomp` 的经验是全局性算法缺陷靠逐节点诊断也定位不到（`pdecomp-design.md:171`）。

## 文件结构

新模块 `src/pst/`，照 `src/pdecomp/` 的结构：

- `pst.hpp`：`ApplyPst(Abc_Frame_t *pAbc)` 声明。命令无参数，所以不引入 `Config` 结构体（`pdecomp` 有 `Config` 是因为它有 `-K`）
- `pst.cpp`：拷贝来的 strash 内核、`Abc_Aig_t_` 镜像、分区感知哈希、主流程、不变量检查
- `CMakeLists.txt`：照 `src/pdecomp/CMakeLists.txt`，链接 `libabc`

改动的现有文件：

- `src/CMakeLists.txt`：`add_subdirectory(pst)` + 加入链接列表
- `src/main.cpp`：`Pst_Command` + `Cmd_CommandAdd(..., "pst", Pst_Command, 1)`，照 `Pdecomp_Command`（`main.cpp:1009`）的风格

不修改 ABC 任何文件。

## 测试与验收

无单元测试框架，验证方式是 build + FoxSYN CLI 运行，照 `pdecomp` 的验收方式：

1. **构建**：`make release`，`Built target FoxSYN`，exit 0
2. **不变量**：`read x.v; st; if -K 6; hpart -N 4; ps; pst; ps` — 确认 hop/cut-net 前后相等，`nd` 显著上升
3. **功能等价**：`write before.blif; pst; write after.blif; cec before.blif after.blif` → `Networks are equivalent`
4. **网络合法性**：`Abc_NtkCheck` 必须通过——这是方案 A 相对 B 的核心卖点。具体确认不出现 `AigCheck: The number of nodes in the structural hashing table is wrong`（硬失败）。`abcAig.c:256` 的 `not in the structural hashing table` 提示会出现，属预期噪声
5. **分区覆盖率**：确认 AIG 里每个 AND 都有有效 `part_id`。判据是 `ps` 的 `pavg × part` 应等于 AND 数 + PI 数（`Abc_NtkGetPartStats` 只统计带有效 `part_id` 的对象，const1 因为留 `NONE` 不计入）。若小于该值，说明有 AND 漏打了 `part_id`
6. **规模覆盖**：ctrl / cavlc / voter / max / arbiter（EPFL，五个不同规模）

## 已知限制 / 后续方向

- **`if` 不保留 `part_id`。** grep 过 `abcIf.c` 和 `src/map/if/`，零个 `PartId` 引用；项目自己的 `agdmap`/`curvemap`/`fox`/`supper` 同样。所以 `pst; if -K 6` 之后分区信息只剩 PI 上那些，所有 LUT 都是 `NONE`。`pst` 保住的边界在 mapping 一步就没了。分区感知 mapping 是独立的后续工作，本设计范围只到 AIG。
- **`pst` 只在标准 LUT netlist 上执行，不支持 `pdecomp` 之后调用。** `pdecomp` 插入的 1-fanin 恒等缓冲（`pdecomp.cpp:122`）在 AIG 里会塌陷：`Abc_NodeStrash` 遇到 Hop 根就是叶子变量的节点时直接返回该 fanin 的 `pCopy`，这个 LUT 在 AIG 里不复存在，消费者转而引用 fanin 的节点——而那个节点属于 fanin 的分区，不是这个 LUT 的分区，cut-net 因此会变、断言会失败。`if -K 6` 本身不产生 1-fanin LUT（ctrl 实测 fanin 直方图 `0:1, 2:1, 3:1, 4:3, 5:19, 6:4`），所以标准流程没问题。
- **`Abc_Aig_t_` 布局镜像是维护负担。** ABC 从上游更新时如果这个结构体变了，镜像会静默失配。根治方向是给 ABC 加一个最小的公开 API（如 `Abc_AigAndCreateForced`），但那需要改 ABC 本体，跟本设计"不改 ABC"的约束冲突，留作后续。
- **不做 `-v` 之类的 verbose 开关。** 初版只有固定的一行统计输出加失败报错，跟 `pdecomp` 一致。
