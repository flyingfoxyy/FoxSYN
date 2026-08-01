# pst Implementation Plan — 分区感知的 structural hashing

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 新增 ABC 命令 `pst`，把带 pdb 分区信息的 LUT netlist 打散成 `ABC_NTK_STRASH` AIG，每个 AND 节点继承宿主 LUT 的 `part_id`，只允许同分区的结构哈希复用。

**Architecture:** 拷贝 `abcStrash.c` 的 strash 内核到 `src/pst/`，把 `Abc_AigAnd` 替换成分区感知版本 `pst_and`。查找走 pst 自己的 `unordered_map`（键含 `part_id`），插入走 ABC 真实的 `pBins`：ABC 表里没有的走公开 `Abc_AigAnd`，撞上别分区同 fanin 节点的走拷贝来的强制建点函数。不修改 ABC 任何文件。

**Tech Stack:** C++23, ABC (子仓库 `src/abc/`), CMake, `Hop_Obj_t` 局部 AIG 表示, `Abc_Aig_t` 全局哈希表

## Global Constraints

- **绝不修改 `src/abc/` 下的任何文件。** 所有需要的 ABC 代码拷贝到 `src/pst/pst.cpp`，注释标注源文件与行号。
- **设计文档** `docs/pst-design.md` 是唯一权威来源；本计划与它冲突时以设计文档为准。
- **代码风格**：4 空格缩进，实现文件里大括号独占一行，`PascalCase` 类型 / `snake_case` 变量与命名空间，文件名小写。参照 `src/pdecomp/pdecomp.cpp`。
- **命名空间**：`fox::pst`（照 `fox::pdecomp`）。
- **硬断言只有两个**：`hop` 相等、`cut-net` 相等。两个 cut-edge 变体只打印、不作判据。
- **`part_id` 归属**：PI 由 `Abc_NtkStartFrom` 自动继承；AND 打宿主 LUT 的 `part_id`；const1 留 `ABC_PART_ID_NONE`。
- **范围止于 AIG**。不碰 mapping，不处理 `if` 不保留 `part_id` 的问题。
- 无单元测试框架。验证方式是 `make release` + 在 `regression/` 下跑 FoxSYN CLI，比对 `ps` 输出与 `cec` 结果。

## 关键事实（实现时必须知道）

**基线行为（已实测，ctrl.v + `if -K 6` + `hpart -N 4`）**：LUT netlist 是 `part=4 hop=1 cut-net=6 cut-edge=16 cut-edge2=106 nd=29`，跑普通 `st` 之后变成 `part=3 hop=0 cut-net=0 cut-edge=0 and=174`，`pavg 2.3 × 3 ≈ 7` 恰好等于 PI 数——**所有 AND 节点的 `part_id` 全丢了**。这就是 `pst` 要解决的问题，也是 Task 5 验证的对照基线。

**`ps` 的标签**：dedup 数印成 `cut-edge`，raw pair 数印成 `cut-edge2`（`abcPrint.c:414-415`）。`pst` 的输出跟 `ps` 对齐。

**`Abc_AigAndLookup` 的平凡返回**（`abcAig.c:410-425`）：`p0==p1` 返回 `p0`；`p0==!p1` 返回 `!const1`；任一侧是 const1 时返回另一侧或 `!const1`。这些不是新建节点，不能进 map、不能打 `part_id`。另外它在 `nFans0==0 || nFans1==0` 时直接返回 NULL（`abcAig.c:441-446`），这意味着**悬空节点查不到**——所以 `pst_and` 不能依赖 lookup 的返回来判断"表里是否真的有这个 key"，只能用它做"要不要强制建点"的启发。这个不精确是安全的：误判成"没有"就走 `Abc_AigAnd`，它内部会再 lookup 一次。

## 文件结构

- **`src/pst/pst.hpp`**（新建，约 15 行）：只声明 `ApplyPst(Abc_Frame_t *)`。命令无参数，不引入 `Config`。
- **`src/pst/pst.cpp`**（新建，约 280 行）：`Abc_Aig_t_` 布局镜像、`pst_hash_key2`、`pst_force_create`、`pst_and`、拷贝改造的 strash 内核三函数、`ApplyPst` 主流程与不变量检查。单文件，因为这些部分共享 `PstMan` 状态、拆开反而要暴露内部结构。
- **`src/pst/CMakeLists.txt`**（新建，9 行）：照 `src/pdecomp/CMakeLists.txt`。
- **`src/CMakeLists.txt`**（修改）：`add_subdirectory(pst)` 加在 `add_subdirectory(pdecomp)` 后；`pst` 加进 `FoxSYN` 的 `target_link_libraries`。
- **`src/main.cpp`**（修改）：`#include "pst/pst.hpp"`；`Pst_Command`；`Cmd_CommandAdd(..., "pst", Pst_Command, 1)`。

---

### Task 1: 模块骨架 — 能编译、能注册、前置检查生效

先让 `pst` 命令存在并正确拒绝非法输入。这一步不做任何 strash 逻辑，目的是把 CMake 接线和命令注册这两件容易出错、又跟算法无关的事单独验证掉。

**Files:**
- Create: `src/pst/pst.hpp`
- Create: `src/pst/pst.cpp`
- Create: `src/pst/CMakeLists.txt`
- Modify: `src/CMakeLists.txt:51`（`add_subdirectory(pdecomp)` 之后）和 `:72`（链接列表里 `pdecomp` 之后）
- Modify: `src/main.cpp:26`（include 区）、`:1047` 之后（新增 `Pst_Command`）、`:1126` 之后（注册）

**Interfaces:**
- Consumes: 无
- Produces: `bool fox::pst::ApplyPst(Abc_Frame_t *pAbc)` — 成功返回 true。Task 2-4 在 `pst.cpp` 内部填充实现。

- [ ] **Step 1: 写 `src/pst/pst.hpp`**

```cpp
#ifndef PST_HPP
#define PST_HPP

#include "misc/util/abc_global.h"
#include "base/main/main.h"

namespace fox::pst {

// Partition-aware structural hashing: LUT netlist -> STRASH AIG, where every
// AND inherits its host LUT's part_id and only same-partition ANDs are shared.
bool ApplyPst(Abc_Frame_t *pAbc);

} // namespace fox::pst

#endif // PST_HPP
```

- [ ] **Step 2: 写 `src/pst/pst.cpp` 的骨架（只有前置检查）**

```cpp
#include "pst.hpp"

#include "base/abc/abc.h"
#include "base/abc/abcPdb.hpp"

#include <cstdio>

namespace fox::pst {

bool ApplyPst(Abc_Frame_t *pAbc)
{
    Abc_Ntk_t *pNtk = Abc_FrameReadNtk(pAbc);
    if (!pNtk)
    {
        printf("pst: network is null\n");
        return false;
    }
    if (!Abc_NtkIsLogic(pNtk))
    {
        printf("pst: network must be logic (not AIG)\n");
        return false;
    }
    if (!pNtk->pPdb)
    {
        printf("pst: no partition database (run hpart first)\n");
        return false;
    }

    printf("pst: skeleton reached, not yet implemented\n");
    return false;
}

} // namespace fox::pst
```

- [ ] **Step 3: 写 `src/pst/CMakeLists.txt`**

```cmake
add_library(pst pst.cpp)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-fexceptions)

target_link_libraries(pst PRIVATE libabc)
target_include_directories(pst PUBLIC ${CMAKE_SOURCE_DIR}/abc/src ${CMAKE_SOURCE_DIR})
```

- [ ] **Step 4: 接线 `src/CMakeLists.txt`**

在 `add_subdirectory(pdecomp)` 那行后面加一行：

```cmake
add_subdirectory(pst)
```

在 `target_link_libraries(FoxSYN PRIVATE ...)` 列表里，`pdecomp` 后面加一行：

```cmake
    pst
```

- [ ] **Step 5: 接线 `src/main.cpp`**

在 `#include "pdecomp/pdecomp.hpp"` 后面加：

```cpp
#include "pst/pst.hpp"
```

在 `Pdecomp_Command` 函数结束后（`usage:` 块的 `return 1; }` 之后）插入：

```cpp
int Pst_Command(Abc_Frame_t *pAbc, int argc, char **argv)
{
    if (argc > 1)
    {
        Abc_Print(-2, "usage: pst\n");
        Abc_Print(-2, "\t           partition-aware structural hashing (LUT netlist -> AIG)\n");
        Abc_Print(-2, "\n");
        return 1;
    }

    return fox::pst::ApplyPst(pAbc) ? 0 : 1;
}
```

在注册块里 `pdecomp` 那行后面加：

```cpp
        Cmd_CommandAdd(Abc_FrameGetGlobalFrame(), "FoxSYN", "pst", Pst_Command, 1);
```

- [ ] **Step 6: 构建**

Run: `cd /home/longfei/FoxSYN && make release 2>&1 | tail -20`
Expected: `[100%] Built target FoxSYN`，exit 0

- [ ] **Step 7: 验证前置检查 — 无网络时报错**

Run:
```bash
cd /home/longfei/FoxSYN/regression && ./FoxSYN -c "pst"
```
Expected: 输出含 `pst: network is null`

- [ ] **Step 8: 验证前置检查 — AIG 网络被拒**

Run:
```bash
cd /home/longfei/FoxSYN/regression && ./FoxSYN -c "read SimpleCircuits/EPFL/ctrl.v; st; pst"
```
Expected: 输出含 `pst: network must be logic (not AIG)`

- [ ] **Step 9: 验证前置检查 — 无 pdb 被拒**

Run:
```bash
cd /home/longfei/FoxSYN/regression && ./FoxSYN -c "read SimpleCircuits/EPFL/ctrl.v; st; if -K 6; pst"
```
Expected: 输出含 `pst: no partition database (run hpart first)`

- [ ] **Step 10: 验证合法输入到达骨架**

Run:
```bash
cd /home/longfei/FoxSYN/regression && ./FoxSYN -c "read SimpleCircuits/EPFL/ctrl.v; st; if -K 6; hpart -N 4; pst"
```
Expected: 输出含 `pst: skeleton reached, not yet implemented`

- [ ] **Step 11: 提交**

```bash
cd /home/longfei/FoxSYN
git add src/pst/ src/CMakeLists.txt src/main.cpp
git commit -m "pst: add module skeleton and command registration"
```

---

### Task 2: 分区感知哈希核心 — `PstMan` + `pst_and`

这是全计划的技术核心：镜像 ABC 的私有 `Abc_Aig_t_` 布局，实现能强制建重复 AND 的 `pst_force_create`，以及带 `part_id` 的查找/插入逻辑。这一步只写这些函数，不接到 strash 流程上——所以还不能端到端测，但代码必须编译通过。

**Files:**
- Modify: `src/pst/pst.cpp`

**Interfaces:**
- Consumes: Task 1 的 `pst.cpp` 骨架
- Produces:
  - `struct PstAigMirror` — `Abc_Aig_t_` 的布局镜像，只读前 5 个字段
  - `struct PstMan { Abc_Ntk_t *pNtkNew; Abc_Aig_t *pMan; std::unordered_map<uint64_t, Abc_Obj_t *> table; int dup_blocked; }`
  - `Abc_Obj_t *pst_and(PstMan &man, Abc_Obj_t *p0, Abc_Obj_t *p1, part_id part)` — Task 3 的 `pst_node_strash_rec` 调用它

- [ ] **Step 1: 加 include 和 `Abc_Aig_t_` 布局镜像**

把 `pst.cpp` 顶部的 include 块替换成：

```cpp
#include "pst.hpp"

#include "base/abc/abc.h"
#include "base/abc/abcPdb.hpp"

#include <cstdint>
#include <cstdio>
#include <unordered_map>

namespace fox::pst {

// ---------------------------------------------------------------------------
// COUPLING WARNING: layout mirror of ABC's private Abc_Aig_t_.
//
// struct Abc_Aig_t_ is defined only in src/abc/src/base/abc/abcAig.c:52 --
// abc.h:118 has just the forward declaration -- and Abc_AigAndCreate is
// static. Creating a second AND with identical fanins (needed when the two
// belong to different partitions) requires writing into pBins directly, which
// requires knowing the layout.
//
// Fields below MUST match abcAig.c:52-70 verbatim, in order. If ABC is updated
// from upstream and that struct gains/loses/reorders a field, this mirror
// silently reads wrong offsets. Only the first five fields are ever accessed;
// the rest exist solely to make the layout correct.
// ---------------------------------------------------------------------------
struct PstAigMirror
{
    Abc_Ntk_t *       pNtkAig;
    Abc_Obj_t *       pConst1;
    Abc_Obj_t **      pBins;
    int               nBins;
    int               nEntries;
    Vec_Ptr_t *       vNodes;
    Vec_Ptr_t *       vStackReplaceOld;
    Vec_Ptr_t *       vStackReplaceNew;
    Vec_Vec_t *       vLevels;
    Vec_Vec_t *       vLevelsR;
    Vec_Ptr_t *       vAddedCells;
    Vec_Ptr_t *       vUpdatedNets;
    int               nStrash0;
    int               nStrash1;
    int               nStrash5;
    int               nStrash2;
};
```

- [ ] **Step 2: 拷贝 `Abc_HashKey2` 和 `Abc_AigAndCreate`**

紧接着上一步的代码加入：

```cpp
// Verbatim copy of Abc_HashKey2 (abcAig.c:90). Must stay bit-identical to
// ABC's, otherwise pst_force_create would file the node in a bin that
// Abc_AigAndLookup/Abc_AigAndDelete would not search.
static unsigned pst_hash_key2(Abc_Obj_t *p0, Abc_Obj_t *p1, int TableSize)
{
    unsigned Key = 0;
    Key ^= Abc_ObjRegular(p0)->Id * 7937;
    Key ^= Abc_ObjRegular(p1)->Id * 2971;
    Key ^= Abc_ObjIsComplement(p0) * 911;
    Key ^= Abc_ObjIsComplement(p1) * 353;
    return Key % TableSize;
}

// Adapted from the static Abc_AigAndCreate (abcAig.c:319). Unconditionally
// creates a new AND and files it in the bin, even when an entry with the same
// (p0, p1) key already exists -- that is the whole point: two ANDs with equal
// fanins but different part_id must coexist.
//
// Two deliberate omissions vs the original:
//   - no Abc_AigResize call: the mirror cannot call the static Abc_AigResize,
//     and forced duplicates are a small minority. Skipping resize only makes a
//     bin chain slightly longer; correctness is unaffected.
//   - no vAddedCells push: that list is for incremental rewriting, which pst
//     does not use.
static Abc_Obj_t *pst_force_create(Abc_Aig_t *pManOpaque, Abc_Obj_t *p0, Abc_Obj_t *p1)
{
    PstAigMirror *pMan = (PstAigMirror *)pManOpaque;
    Abc_Obj_t *pAnd;
    unsigned Key;

    // order the arguments (same convention as Abc_AigAndCreate)
    if (Abc_ObjRegular(p0)->Id > Abc_ObjRegular(p1)->Id)
        pAnd = p0, p0 = p1, p1 = pAnd;

    pAnd = Abc_NtkCreateNode(pMan->pNtkAig);
    Abc_ObjAddFanin(pAnd, p0);
    Abc_ObjAddFanin(pAnd, p1);
    pAnd->Level  = 1 + Abc_MaxInt(Abc_ObjRegular(p0)->Level, Abc_ObjRegular(p1)->Level);
    pAnd->fExor  = Abc_NodeIsExorType(pAnd);
    pAnd->fPhase = (Abc_ObjIsComplement(p0) ^ Abc_ObjRegular(p0)->fPhase)
                 & (Abc_ObjIsComplement(p1) ^ Abc_ObjRegular(p1)->fPhase);

    Key = pst_hash_key2(p0, p1, pMan->nBins);
    pAnd->pNext      = pMan->pBins[Key];
    pMan->pBins[Key] = pAnd;
    pMan->nEntries++;
    pAnd->pCopy = NULL;
    return pAnd;
}
```

- [ ] **Step 3: 加 `PstMan` 和查找键编码**

```cpp
struct PstMan
{
    Abc_Ntk_t *pNtkNew = NULL;
    Abc_Aig_t *pMan    = NULL;
    std::unordered_map<uint64_t, Abc_Obj_t *> table;
    int dup_blocked = 0;
};

// Key layout: [ id0:27 | c0:1 | id1:27 | c1:1 | part:8 ]. Object ids fit in 27
// bits (134M objects) with room to spare for any netlist pst will see; part_id
// is a uint8_t (abc.h:124). Callers must pass p0/p1 already ordered by id.
static inline uint64_t pst_make_key(Abc_Obj_t *p0, Abc_Obj_t *p1, part_id part)
{
    uint64_t id0 = (uint64_t)Abc_ObjRegular(p0)->Id;
    uint64_t id1 = (uint64_t)Abc_ObjRegular(p1)->Id;
    uint64_t c0  = Abc_ObjIsComplement(p0) ? 1u : 0u;
    uint64_t c1  = Abc_ObjIsComplement(p1) ? 1u : 0u;
    return (id0 << 37) | (c0 << 36) | (id1 << 9) | (c1 << 8) | (uint64_t)part;
}
```

- [ ] **Step 4: 实现 `pst_and`**

```cpp
// Partition-aware replacement for Abc_AigAnd. Reuse happens only among ANDs
// with the same part_id; a same-fanin AND belonging to another partition
// forces a duplicate node instead of being shared.
static Abc_Obj_t *pst_and(PstMan &man, Abc_Obj_t *p0, Abc_Obj_t *p1, part_id part)
{
    // Trivial cases resolve to an existing object or a constant, not to a new
    // AND -- they get no part_id and no table entry. Mirrors the head of
    // Abc_AigAndLookup (abcAig.c:410-425).
    Abc_Obj_t *pConst1 = Abc_AigConst1(man.pNtkNew);
    if (p0 == p1)
        return p0;
    if (p0 == Abc_ObjNot(p1))
        return Abc_ObjNot(pConst1);
    if (Abc_ObjRegular(p0) == pConst1)
        return (p0 == pConst1) ? p1 : Abc_ObjNot(pConst1);
    if (Abc_ObjRegular(p1) == pConst1)
        return (p1 == pConst1) ? p0 : Abc_ObjNot(pConst1);

    // Order the arguments so the key is canonical, matching what
    // Abc_AigAndCreate/pst_force_create will store.
    if (Abc_ObjRegular(p0)->Id > Abc_ObjRegular(p1)->Id)
    {
        Abc_Obj_t *pTemp = p0;
        p0 = p1;
        p1 = pTemp;
    }

    uint64_t key = pst_make_key(p0, p1, part);
    auto it = man.table.find(key);
    if (it != man.table.end())
        return it->second; // same-partition reuse

    Abc_Obj_t *pAnd;
    if (Abc_AigAndLookup(man.pMan, p0, p1) == NULL)
    {
        // Not in ABC's table (or dangling, which lookup also reports as NULL):
        // the public API handles creation, resizing, and its own re-lookup.
        pAnd = Abc_AigAnd(man.pMan, p0, p1);
    }
    else
    {
        // ABC already has this (p0, p1) but under a different part_id, so the
        // node cannot be shared. Force a duplicate.
        pAnd = pst_force_create(man.pMan, p0, p1);
        man.dup_blocked++;
    }

    Abc_ObjSetPartId(pAnd, part);
    man.table[key] = pAnd;
    return pAnd;
}
```

- [ ] **Step 5: 构建，确认这些函数能编译**

Run: `cd /home/longfei/FoxSYN && make release 2>&1 | grep -E "error|warning: unused function|Built target FoxSYN" | head -20`
Expected: 出现 `Built target FoxSYN`，无 `error`。可能出现 `pst_and`/`pst_force_create` 未使用的告警，那是预期的（Task 3 才接上）。

- [ ] **Step 6: 验证布局镜像的字段数与 ABC 一致**

Run:
```bash
cd /home/longfei/FoxSYN
echo "ABC struct fields:"; sed -n '52,70p' src/abc/src/base/abc/abcAig.c | grep -cE '^\s+(Abc_|Vec_|int)'
echo "mirror fields:"; sed -n '/^struct PstAigMirror$/,/^};/p' src/pst/pst.cpp | grep -cE '^\s+(Abc_|Vec_|int)'
```
Expected: 两个数字相等（16）。这是 Task 2 唯一能做的镜像正确性检查——真正的验证在 Task 5 的 `Abc_NtkCheck`。

- [ ] **Step 7: 提交**

```bash
cd /home/longfei/FoxSYN
git add src/pst/pst.cpp
git commit -m "pst: add partition-aware AND hashing over ABC's aig table"
```

---

### Task 3: strash 内核 — 拷贝改造 `Abc_NodeStrash` 三函数

把 `abcStrash.c` 的三个函数拷进来，唯一实质改动是让 AND 的构造走 `pst_and` 并携带宿主 LUT 的 `part_id`。

**Files:**
- Modify: `src/pst/pst.cpp`

**Interfaces:**
- Consumes: Task 2 的 `pst_and`、`PstMan`
- Produces:
  - `void pst_node_strash_rec(PstMan &man, Hop_Obj_t *pObj, part_id part)`
  - `Abc_Obj_t *pst_node_strash(PstMan &man, Abc_Obj_t *pNodeOld)`
  - `void pst_strash_perform(PstMan &man, Abc_Ntk_t *pNtkOld)` — Task 4 的 `ApplyPst` 调用它

- [ ] **Step 1: 拷贝改造 `Abc_NodeStrash_rec`**

在 `pst_and` 之后插入：

```cpp
// Adapted from Abc_NodeStrash_rec (abcStrash.c:445). Only change: Abc_AigAnd
// -> pst_and, threading the host LUT's part_id down the whole Hop cone.
static void pst_node_strash_rec(PstMan &man, Hop_Obj_t *pObj, part_id part)
{
    assert(!Hop_IsComplement(pObj));
    if (!Hop_ObjIsNode(pObj) || Hop_ObjIsMarkA(pObj))
        return;
    pst_node_strash_rec(man, Hop_ObjFanin0(pObj), part);
    pst_node_strash_rec(man, Hop_ObjFanin1(pObj), part);
    pObj->pData = pst_and(man, (Abc_Obj_t *)Hop_ObjChild0Copy(pObj),
                                (Abc_Obj_t *)Hop_ObjChild1Copy(pObj), part);
    assert(!Hop_ObjIsMarkA(pObj)); // loop detection
    Hop_ObjSetMarkA(pObj);
}
```

- [ ] **Step 2: 拷贝改造 `Abc_NodeStrash`**

```cpp
// Adapted from Abc_NodeStrash (abcStrash.c:468). Two changes: the part_id is
// read once from the old node and threaded through the recursion, and the
// fRecord / Abc_NtkRec branch (already commented out upstream) is dropped.
static Abc_Obj_t *pst_node_strash(PstMan &man, Abc_Obj_t *pNodeOld)
{
    Hop_Man_t *pMan;
    Hop_Obj_t *pRoot;
    Abc_Obj_t *pFanin;
    int i;
    assert(Abc_ObjIsNode(pNodeOld));
    assert(Abc_NtkHasAig(pNodeOld->pNtk) && !Abc_NtkIsStrash(pNodeOld->pNtk));

    // Every AND in this LUT's cone inherits this one part_id. Same-partition
    // sharing (including across LUTs, since man.table is global) is unaffected;
    // only cross-partition merging is blocked.
    part_id part = Abc_ObjGetPartId(pNodeOld);

    pMan  = (Hop_Man_t *)pNodeOld->pNtk->pManFunc;
    pRoot = (Hop_Obj_t *)pNodeOld->pData;

    // Constant LUTs map onto the network's single shared const1, which cannot
    // be split per partition -- it keeps ABC_PART_ID_NONE.
    if (Abc_NodeIsConst(pNodeOld) || Hop_Regular(pRoot) == Hop_ManConst1(pMan))
        return Abc_ObjNotCond(Abc_AigConst1(man.pNtkNew), Hop_IsComplement(pRoot));

    // set elementary variables
    Abc_ObjForEachFanin(pNodeOld, pFanin, i)
        Hop_IthVar(pMan, i)->pData = pFanin->pCopy;

    pst_node_strash_rec(man, Hop_Regular(pRoot), part);
    Hop_ConeUnmark_rec(Hop_Regular(pRoot));
    return Abc_ObjNotCond((Abc_Obj_t *)Hop_Regular(pRoot)->pData, Hop_IsComplement(pRoot));
}
```

- [ ] **Step 3: 拷贝改造 `Abc_NtkStrashPerform`**

```cpp
// Adapted from Abc_NtkStrashPerform (abcStrash.c:413). fAllNodes is fixed to 0
// (matching what the st command passes by default, abc.c:3845) and fRecord is
// dropped along with the record branch.
static void pst_strash_perform(PstMan &man, Abc_Ntk_t *pNtkOld)
{
    Vec_Ptr_t *vNodes;
    Abc_Obj_t *pNodeOld;
    int i;
    assert(Abc_NtkIsLogic(pNtkOld));
    assert(Abc_NtkIsStrash(man.pNtkNew));
    vNodes = Abc_NtkDfsIter(pNtkOld, 0);
    Vec_PtrForEachEntry(Abc_Obj_t *, vNodes, pNodeOld, i)
    {
        if (Abc_ObjIsBarBuf(pNodeOld))
            pNodeOld->pCopy = Abc_ObjChild0Copy(pNodeOld);
        else
            pNodeOld->pCopy = pst_node_strash(man, pNodeOld);
    }
    Vec_PtrFree(vNodes);
}
```

- [ ] **Step 4: 构建**

Run: `cd /home/longfei/FoxSYN && make release 2>&1 | grep -E "error|Built target FoxSYN" | head -20`
Expected: `Built target FoxSYN`，无 `error`

- [ ] **Step 5: 提交**

```bash
cd /home/longfei/FoxSYN
git add src/pst/pst.cpp
git commit -m "pst: add partition-threading strash kernel"
```

---

### Task 4: 主流程 — 建网络、搬 pdb、断言、报告

把 Task 2/3 接起来，替换掉 Task 1 的骨架。这一步跑完 `pst` 就端到端可用了。

**Files:**
- Modify: `src/pst/pst.cpp`

**Interfaces:**
- Consumes: Task 3 的 `pst_strash_perform`
- Produces: 完整的 `ApplyPst`，输出格式见下

- [ ] **Step 1: 用完整实现替换骨架版 `ApplyPst`**

把 Task 1 写的 `ApplyPst`（从 `bool ApplyPst(Abc_Frame_t *pAbc)` 到它的结束大括号）整体替换为：

```cpp
bool ApplyPst(Abc_Frame_t *pAbc)
{
    Abc_Ntk_t *pNtk = Abc_FrameReadNtk(pAbc);
    if (!pNtk)
    {
        printf("pst: network is null\n");
        return false;
    }
    if (!Abc_NtkIsLogic(pNtk))
    {
        printf("pst: network must be logic (not AIG)\n");
        return false;
    }
    if (!pNtk->pPdb)
    {
        printf("pst: no partition database (run hpart first)\n");
        return false;
    }

    int initial_hop     = Abc_NtkComputeHopNum(pNtk);
    int initial_cutnet  = Abc_NtkComputeCutSize(pNtk);
    int initial_cutedge = Abc_NtkComputeCutEdgeDedupNum(pNtk);
    int initial_cutedge2 = Abc_NtkComputeCutEdgeNum(pNtk);
    int initial_nodes   = Abc_NtkNodeNum(pNtk);

    // Degenerate LUTs (a single fanin, or a Hop root that is just a leaf
    // variable) vanish in an AIG: Abc_NodeStrash returns the fanin's pCopy
    // directly, so consumers end up referencing a node in the *fanin's*
    // partition. That legitimately changes cut-net and will trip the assertion
    // below, so report the count up front to make diagnosis immediate.
    // if -K 6 does not emit such LUTs; pdecomp's cross-partition identity
    // buffers (pdecomp.cpp:122) do -- pst does not support running after it.
    int degenerate = 0;
    Abc_Obj_t *pObj;
    int i;
    Abc_NtkForEachNode(pNtk, pObj, i)
        if (Abc_ObjFaninNum(pObj) == 1)
            degenerate++;
    if (degenerate)
        printf("pst: warning: %d single-fanin node(s); these collapse in an AIG "
               "and may change cut-net\n", degenerate);

    if (!Abc_NtkToAig(pNtk))
    {
        printf("pst: converting node functions to AIG failed\n");
        return false;
    }

    PstMan man;
    man.pNtkNew = Abc_NtkStartFrom(pNtk, ABC_NTK_STRASH, ABC_FUNC_AIG);
    man.pMan    = (Abc_Aig_t *)man.pNtkNew->pManFunc;

    pst_strash_perform(man, pNtk);
    Abc_NtkFinalize(pNtk, man.pNtkNew);
    if (pNtk->vNameIds)
        Abc_NtkTransferNameIds(pNtk, man.pNtkNew);

    // Abc_NtkStartFrom -> Abc_NtkDupObj already carried each PI's part_id
    // (abcObj.c:399), and pst_and stamped every AND. balance_pct has no C-level
    // API, so copy it directly (pdecomp.cpp:4 sets the include precedent).
    if (man.pNtkNew->pPdb && pNtk->pPdb->balance_pct() >= 0)
        man.pNtkNew->pPdb->set_balance_pct(pNtk->pPdb->balance_pct());

    int final_hop      = Abc_NtkComputeHopNum(man.pNtkNew);
    int final_cutnet   = Abc_NtkComputeCutSize(man.pNtkNew);
    int final_cutedge  = Abc_NtkComputeCutEdgeDedupNum(man.pNtkNew);
    int final_cutedge2 = Abc_NtkComputeCutEdgeNum(man.pNtkNew);
    int final_nodes    = Abc_NtkNodeNum(man.pNtkNew);

    // Only hop and cut-net are assertions. Both depend solely on which
    // partitions a signal reaches, not on how much logic sits in between, so
    // expanding a LUT into same-partition ANDs cannot move them.
    if (final_hop != initial_hop || final_cutnet != initial_cutnet)
    {
        printf("pst: partition invariant violated (hop %d->%d, cut-net %d->%d), aborting\n",
               initial_hop, final_hop, initial_cutnet, final_cutnet);
        Abc_NtkDelete(man.pNtkNew);
        return false;
    }

    if (!Abc_NtkCheck(man.pNtkNew))
    {
        printf("pst: network check failed, aborting\n");
        Abc_NtkDelete(man.pNtkNew);
        return false;
    }

    printf("pst: strashed to AIG (hop=%d, cut-net=%d)\n", initial_hop, initial_cutnet);
    // Labels follow ps (abcPrint.c:414-415): cut-edge is the deduplicated
    // count, cut-edge2 the raw (driver, consumer) pair count. cut-edge2 must
    // rise: an AIG cannot hold pdecomp-style identity buffers, so a
    // cross-partition fanin referenced by several ANDs multiplies its edges.
    // cut-edge may legitimately fall when a vacuous fanin disappears.
    printf("pst: cut-edge %d -> %d, cut-edge2 %d -> %d\n",
           initial_cutedge, final_cutedge, initial_cutedge2, final_cutedge2);
    printf("pst: nodes %d -> %d, dup-blocked %d\n",
           initial_nodes, final_nodes, man.dup_blocked);

    Abc_FrameReplaceCurrentNetwork(pAbc, man.pNtkNew);
    return true;
}
```

- [ ] **Step 2: 构建**

Run: `cd /home/longfei/FoxSYN && make release 2>&1 | grep -E "error|Built target FoxSYN" | head -20`
Expected: `Built target FoxSYN`，无 `error`

- [ ] **Step 3: 首跑 ctrl.v，看是否通过断言**

Run:
```bash
cd /home/longfei/FoxSYN/regression && ./FoxSYN -c "read SimpleCircuits/EPFL/ctrl.v; st; if -K 6; hpart -N 4; ps; pst; ps" 2>&1 | tail -14
```
Expected: 出现 `pst: strashed to AIG (hop=1, cut-net=6)`。断言基线来自实测：ctrl 的 LUT netlist 是 `hop=1 cut-net=6`。若报 `partition invariant violated`，进入 Task 4 的 Step 4 排查；若报 `network check failed`，说明 Task 2 的镜像有问题，回到 Task 2 Step 6。

- [ ] **Step 4: 若断言失败，按此顺序排查（成功则跳过）**

1. 先看有没有 `single-fanin node(s)` 告警——若有，说明输入不是标准 `if -K 6` 产物，换干净输入重试
2. `cut-net` 变了但 `hop` 没变：查 const1 是否被误打了 `part_id`（`pst_node_strash` 的常量分支不应调 `Abc_ObjSetPartId`）
3. `hop` 变了：查 `pst_and` 是否漏打 `part_id`，特别是 `Abc_AigAnd` 返回既有节点（被 ABC 内部 lookup 命中）时——那种情况下 `Abc_ObjSetPartId` 会把已有节点的 `part_id` **覆盖**成当前 part。这是真实风险：`Abc_AigAnd` 内部先 lookup，如果 ABC 表里有个悬空的同 fanin 节点（`Abc_AigAndLookup` 因 `nFans==0` 对我们返回 NULL，但 `Abc_AigAnd` 里的 lookup 同样返回 NULL 所以会新建）……确认方式是在 `pst_and` 里 `Abc_AigAnd` 分支后加临时断言：
   ```cpp
   part_id existing = Abc_ObjGetPartId(pAnd);
   assert(existing == ABC_PART_ID_NONE || existing == part);
   ```
   若断言触发，改成"命中已有节点且 part 不同则走 `pst_force_create`"

- [ ] **Step 5: 提交**

```bash
cd /home/longfei/FoxSYN
git add src/pst/pst.cpp
git commit -m "pst: wire up main flow with invariant assertions"
```

---

### Task 5: 验收 — 五个 case 上的等价性、合法性、分区覆盖率

Task 4 只验了 ctrl 一个 case。这一步做设计文档"测试与验收"一节列的全部六项，覆盖五个不同规模的 EPFL case。

**Files:**
- 无代码改动（发现问题则回到对应 Task 修）

**Interfaces:**
- Consumes: Task 4 完成的 `pst`
- Produces: 验收结论；若全通过则更新 `docs/pst-design.md` 记录实测数据

- [ ] **Step 1: 功能等价性 — cec，五个 case**

Run:
```bash
cd /home/longfei/FoxSYN/regression
for c in ctrl cavlc voter max arbiter; do
  echo "=== $c"
  timeout 900 ./FoxSYN -c "read SimpleCircuits/EPFL/$c.v; st; if -K 6; hpart -N 4; write /tmp/pst_b.blif; pst; write /tmp/pst_a.blif; cec /tmp/pst_b.blif /tmp/pst_a.blif" 2>&1 | grep -E "pst:|equivalent|NOT EQUIVALENT|Networks are"
done
```
Expected: 每个 case 都出现 `pst: strashed to AIG` 和 `Networks are equivalent`

- [ ] **Step 2: 网络合法性 — 确认无 AigCheck 硬失败**

Run:
```bash
cd /home/longfei/FoxSYN/regression
for c in ctrl cavlc voter max arbiter; do
  echo "=== $c"
  timeout 900 ./FoxSYN -c "read SimpleCircuits/EPFL/$c.v; st; if -K 6; hpart -N 4; pst" 2>&1 | grep -E "AigCheck|check failed|pst: strashed"
done
```
Expected: 每个 case 出现 `pst: strashed to AIG`；**不能**出现 `The number of nodes in the structural hashing table is wrong`（那是硬失败）或 `pst: network check failed`。可能出现 `not in the structural hashing table`——那是 `abcAig.c:256` 的 `printf`、不返回失败，是被遮蔽的重复节点造成的预期噪声。

- [ ] **Step 3: 不变量 + 节点增长**

Run:
```bash
cd /home/longfei/FoxSYN/regression
for c in ctrl cavlc voter max arbiter; do
  echo "=== $c"
  timeout 900 ./FoxSYN -c "read SimpleCircuits/EPFL/$c.v; st; if -K 6; hpart -N 4; ps; pst; ps" 2>&1 | grep -E "pst:|hop =|nd =|and ="
done
```
Expected: 每个 case 的 `pst:` 报告里 hop/cut-net 通过；`ps` 前后 `hop` 与 `cut-net` 数值相等；`nd`（LUT 数）到 `and`（AND 数）显著增长

- [ ] **Step 4: 分区覆盖率 — 每个 AND 都有 part_id**

判据（设计文档"测试与验收"第 5 项）：`pavg × part` 应等于 AND 数 + PI 数。`Abc_NtkGetPartStats` 只统计带有效 `part_id` 的对象，const1 留 `NONE` 故不计入。

Run:
```bash
cd /home/longfei/FoxSYN/regression
timeout 300 ./FoxSYN -c "read SimpleCircuits/EPFL/ctrl.v; st; if -K 6; hpart -N 4; pst; ps" 2>&1 | grep -E "i/o|pavg|pst: nodes"
```
Expected: 手算 `pavg × part` ≈ `and` + PI 数（ctrl 的 PI 是 7）。若明显小于，说明有 AND 漏打 `part_id`，回 Task 2 查 `pst_and`。

- [ ] **Step 5: 对照普通 st，确认 pst 真的解决了问题**

Run:
```bash
cd /home/longfei/FoxSYN/regression
echo "--- plain st (baseline: part drops, hop/cut-net go to 0)"
timeout 300 ./FoxSYN -c "read SimpleCircuits/EPFL/ctrl.v; st; if -K 6; hpart -N 4; st; ps" 2>&1 | grep -E "part =|pavg"
echo "--- pst (should preserve part=4, hop=1, cut-net=6)"
timeout 300 ./FoxSYN -c "read SimpleCircuits/EPFL/ctrl.v; st; if -K 6; hpart -N 4; pst; ps" 2>&1 | grep -E "part =|pavg"
```
Expected: 普通 `st` 那行是 `part=3 hop=0 cut-net=0 pavg=2.3`（已实测的基线，AND 全丢 `part_id`）；`pst` 那行保住 `part=4 hop=1 cut-net=6`，`pavg` 远大于 2.3

- [ ] **Step 6: 把实测数据补进设计文档**

在 `docs/pst-design.md` 的"测试与验收"一节末尾追加一小节，填入 Step 1-5 的真实数字：

```markdown
### 实测结果

| case | LUT nd | AIG and | dup-blocked | hop | cut-net | cut-edge | cut-edge2 | cec |
|---|---|---|---|---|---|---|---|---|
| ctrl | ... | ... | ... | ... | ... | ...->... | ...->... | EQ |
（voter/cavlc/max/arbiter 同）

对照普通 `st`：ctrl 上 `st` 之后 `part` 4->3、`hop` 1->0、`cut-net` 6->0，`pavg 2.3 × 3 ≈ 7` 恰等于 PI 数，即所有 AND 的 `part_id` 全丢；`pst` 保住 `part=4 hop=1 cut-net=6`。
```

- [ ] **Step 7: 提交**

```bash
cd /home/longfei/FoxSYN
git add docs/pst-design.md
git commit -m "pst: record measured results across five EPFL cases"
```

---

## Self-Review

**Spec coverage** — 设计文档各节到任务的映射：

| 设计文档章节 | 覆盖任务 |
|---|---|
| 命令接口（无参数、三条前置检查） | Task 1 Step 2/5，Step 7-9 验证 |
| 构造新网络而非原地修改 | Task 4 Step 1（`Abc_NtkStartFrom` + 失败时 `Abc_NtkDelete`） |
| 拷贝 ABC 代码表格（六项） | Task 2 Step 1-2（结构体镜像、HashKey2、AigAndCreate），Task 3 Step 1-3（三个 strash 函数），Task 4 Step 1（顶层流程） |
| 为什么必须镜像私有结构体 + 脆弱点注释 | Task 2 Step 1 的 COUPLING WARNING 注释 |
| 方案 A/B/C 权衡 | 已在设计文档里定案，Task 5 Step 2 验证方案 A 的核心卖点（`Abc_NtkCheck` 通过） |
| 分区感知 AND 构造伪码 | Task 2 Step 4 |
| 平凡情况不进 map | Task 2 Step 4 开头四个 if |
| part_id 归属（PI/AND/const1/PO） | Task 3 Step 2（const1 分支不打 part_id）、Task 2 Step 4（AND）、Task 4 Step 1 注释（PI 自动继承） |
| balance_pct 手工搬 | Task 4 Step 1 |
| 节点删除的正确性 | 无需代码，Task 2 Step 2 注释里说明 HashKey2 必须逐位一致 |
| 硬断言 hop + cut-net | Task 4 Step 1 |
| 两个 cut-edge 只报告 | Task 4 Step 1 的第二条 printf |
| dup-blocked 诊断 | Task 2 Step 4（计数）、Task 4 Step 1（打印） |
| 文件结构 | Task 1 全部 |
| 测试与验收六项 | Task 5 Step 1-4（等价性/合法性/不变量/覆盖率），Task 1 Step 6 构建，Task 5 Step 1 规模覆盖 |
| 已知限制：pdecomp 之后不支持 | Task 4 Step 1 的 degenerate LUT 告警 + 注释 |

无缺口。

**Placeholder scan** — 全部步骤含可直接粘贴的代码或可直接执行的命令。Task 5 Step 6 的表格有 `...` 占位，但那是待填的实测数据（执行时才有值），不是未定的设计决策。

**Type consistency** — `PstMan` 字段名（`pNtkNew`/`pMan`/`table`/`dup_blocked`）在 Task 2 定义、Task 3/4 使用，一致。`pst_and` 签名 `(PstMan &, Abc_Obj_t *, Abc_Obj_t *, part_id)` 在 Task 2 定义、Task 3 Step 1 调用，一致。`pst_force_create` 收 `Abc_Aig_t *` 并在内部转 `PstAigMirror *`，跟 `man.pMan` 的类型一致。

**一处执行时需留意的风险**，已写进 Task 4 Step 4：`Abc_AigAnd` 可能返回 ABC 内部 lookup 命中的既有节点，此时 `Abc_ObjSetPartId` 会覆盖它原有的 `part_id`。理论上 `pst_and` 先做的 `Abc_AigAndLookup` 已经把这种情况分流到 `pst_force_create` 了，但两次 lookup 之间对悬空节点的判定不同（`nFans==0` 时返回 NULL），所以留了临时断言的排查方案。
