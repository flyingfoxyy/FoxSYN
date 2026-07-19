# csr3 Phase 0 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a read-only `csr3` ABC command that measures, with trustworthy numbers, how much combinational-SDC "water" (recoverable redundant wires) hides in the cross-partition interconnect after `hpart -N 2`.

**Architecture:** Mirror the existing `csr` module. For each direction (0→1, 1→0): collect crossing signals → extract partition-bounded fanin cones → group by support overlap (Jaccard) → build a per-group cone sub-network → bit-parallel random simulation prefilter → exact All-SAT count of reachable output combinations `m` → report `gain = k − ⌈log₂ m⌉`. Never mutates the network.

**Tech Stack:** C++23, vendored ABC (`Abc_Ntk_t`, `Abc_NtkStrash`, `Abc_NtkToDar`→`Aig_Man_t`, `Cnf_Derive`+`sat_solver` for All-SAT, `Gia_ManFromAig`+`Gia_ManSimPatSim` for bit-parallel sim), CMake.

## Global Constraints

- **Design spec:** `docs/csr3-phase0-design.md` (this plan implements it). Methodology: `docs/csr3.md`.
- **Language/standard:** C++23, `-fexceptions` (mirror `src/csr/CMakeLists.txt`).
- **Style:** 4-space indent, braces on own line in `.cpp`, `PascalCase` types, `snake_case` functions/vars/namespaces, lowercase filenames. Match surrounding code; keep formatting changes narrow. (`CLAUDE.md`)
- **Namespace:** `fox::csr3`.
- **Read-only:** Phase 0 never modifies `pNtk`, never registers undo, never changes partition ids.
- **N=2 only:** guard `num_parts() == 2`; error out otherwise.
- **Partition sentinel:** `part_id` is `uint8_t`; `ABC_PART_ID_NONE == 0xFF`. Get a node's partition via `Abc_ObjGetPartId(pObj)`.
- **Correctness invariant (never violate):** cone leaves (PI/FF-Q/opposite-partition nodes) are free variables ⇒ computed `m` is an upper bound on the true reachable count ⇒ `gain` is a lower bound on recoverable wires ⇒ the tool never over-reports. Report wording is **detected-floor**, not zero.
- **No unit-test framework:** follow the `src/test_csr2.cpp` idiom (hand-built `Abc_Ntk_t`, custom `Expect*` helpers, a `test_csr3` executable wired into `src/CMakeLists.txt`). Build with `make` (release tree in `release/`).
- **Build/verify command:** `make 2>&1 | tail -20` then `./release/test_csr3`.

---

## File Structure

```
src/csr3/
  CMakeLists.txt   — add_library(csr3 csr3.cpp); link libabc + timer
  csr3.hpp         — namespace fox::csr3: Config, GroupResult, DirReport, RunCsr3()
  csr3_internal.hpp— internal unit-function decls (exposed only for test_csr3)
  csr3.cpp         — all units + orchestration
src/test_csr3.cpp  — standalone test driver (test_csr2 idiom)
```

Edits to existing files:
- `src/CMakeLists.txt` — `add_subdirectory(csr3)` (line ~48), add `csr3` to FoxSYN link list (line ~72), add `test_csr3` executable block (after the `test_csr2` block ~line 109).
- `src/main.cpp` — `#include "csr3/csr3.hpp"` (line ~24), add `Csr3_Command` function (after `Csr2_Command`, ~line 860), register it (after line 1072).

---

## Interfaces (shared across tasks)

These types/signatures are fixed here so every task uses identical names. Defined in `csr3.hpp` (public) and `csr3_internal.hpp` (unit functions).

`csr3.hpp`:
```cpp
#ifndef CSR3_HPP
#define CSR3_HPP

#include "misc/util/abc_global.h"
#include "base/main/main.h"

namespace fox::csr3 {

struct Config {
    int  jaccard_pct = 30;      // -J: Jaccard grouping threshold, percent (1-99)
    int  max_lines   = 16;      // -M: max lines per group (k cap)
    int  sim_words   = 16;      // -P: random-sim words (x64 patterns each)
    int  btlimit     = 100000;  // -B: SAT backtrack limit per solve
    bool self_check  = false;   // -c: exhaustive cross-check for |support|<=16 groups
    bool verbose     = false;   // -v
};

bool RunCsr3(Abc_Ntk_t *pNtk, const Config &cfg);

} // namespace fox::csr3
#endif // CSR3_HPP
```

`csr3_internal.hpp` (unit functions, for testing):
```cpp
#ifndef CSR3_INTERNAL_HPP
#define CSR3_INTERNAL_HPP

#include <vector>
#include "base/abc/abc.h"

namespace fox::csr3 {

// A crossing line plus its partition-bounded support (sorted leaf ObjIds).
struct Line {
    Abc_Obj_t         *driver = nullptr;
    std::vector<int>   support;   // sorted, unique leaf ObjIds
};

struct Group {
    std::vector<Abc_Obj_t*> lines;   // <= max_lines drivers, k = lines.size()
};

// Per-group measurement result.
struct GroupResult {
    int  k           = 0;      // number of lines in the group
    long m           = 0;      // reachable output combinations (>=1); may be capped (see prefiltered)
    int  gain        = 0;      // k - ceil_log2(m), >= 0
    int  support_size= 0;      // |union of supports|
    bool prefiltered = false;  // true = simulation ruled it out (m treated as 2^k, gain=0)
};

// Task 2
std::vector<Abc_Obj_t*> collect_crossing_signals(Abc_Ntk_t *pNtk, int srcPart);
// Task 3
std::vector<int>        extract_support_partition_aware(Abc_Obj_t *line, int srcPart);
bool                    is_cone_leaf(Abc_Obj_t *pObj, int srcPart);
// Task 4
std::vector<Group>      group_by_jaccard(const std::vector<Line> &lines, int jaccardPct, int kmax);
// Task 5
Abc_Ntk_t *             build_group_cone_ntk(const std::vector<Abc_Obj_t*> &lines, int srcPart);
// Task 6
long                    simulate_prefilter(Abc_Ntk_t *pCone, int k, int nWords);
// Task 7
long                    count_m_sat(Abc_Ntk_t *pCone, int k, int btlimit);      // exact m; > (long)1<<(k-1) => early-exit sentinel; -1 => timeout
long                    count_m_exhaustive(Abc_Ntk_t *pCone, int k);            // exact m via full 2^support sim (support<=16)

// helpers
int  ceil_log2(long m);   // ceil(log2(m)); ceil_log2(1)=0

} // namespace fox::csr3
#endif // CSR3_INTERNAL_HPP
```

---

### Task 1: Module scaffold, command registration, build wiring

Gets a green build with a `csr3` command that parses options, fires all four guards, and prints a stub line. No measurement yet.

**Files:**
- Create: `src/csr3/csr3.hpp` (content above)
- Create: `src/csr3/csr3_internal.hpp` (content above)
- Create: `src/csr3/csr3.cpp`
- Create: `src/csr3/CMakeLists.txt`
- Create: `src/test_csr3.cpp`
- Modify: `src/CMakeLists.txt` (add_subdirectory + FoxSYN link + test_csr3 exe)
- Modify: `src/main.cpp` (include + Csr3_Command + registration)

**Interfaces:**
- Produces: `fox::csr3::RunCsr3(Abc_Ntk_t*, const Config&)`, `fox::csr3::ceil_log2(long)`.

- [ ] **Step 1: Write `src/csr3/CMakeLists.txt`** (mirror `src/csr/CMakeLists.txt`; no `cpr` link)

```cmake
add_library(csr3 csr3.cpp)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-fexceptions)

target_link_libraries(csr3 PRIVATE libabc timer)
target_include_directories(csr3 PUBLIC ${CMAKE_SOURCE_DIR}/abc/src ${CMAKE_SOURCE_DIR})
```

- [ ] **Step 2: Write `src/csr3/csr3.hpp` and `src/csr3/csr3_internal.hpp`** using the exact content from the Interfaces section above.

- [ ] **Step 3: Write `src/csr3/csr3.cpp` scaffold** — `ceil_log2` + stub `RunCsr3` that guards and prints.

```cpp
#include "csr3/csr3.hpp"
#include "csr3/csr3_internal.hpp"

#include <cstdio>

#include "base/abc/abc.h"
#include "base/abc/abcPdb.hpp"

namespace fox::csr3 {

int ceil_log2(long m)
{
    if (m <= 1)
        return 0;
    int bits = 0;
    long v = m - 1;
    while (v > 0) { v >>= 1; ++bits; }
    return bits;
}

bool RunCsr3(Abc_Ntk_t *pNtk, const Config &cfg)
{
    if (!pNtk) { printf("csr3: current network is empty\n"); return false; }
    if (!Abc_NtkIsLogic(pNtk)) { printf("csr3: network must be logic (not AIG)\n"); return false; }
    if (!pNtk->pPdb) { printf("csr3: no partition database (run hpart first)\n"); return false; }
    if (Abc_NtkPdb(pNtk)->num_parts() != 2) {
        printf("csr3: v1 only supports N=2 partitions (got %d)\n", Abc_NtkPdb(pNtk)->num_parts());
        return false;
    }
    (void)cfg;
    printf("csr3: scaffold OK (measurement not yet implemented)\n");
    return true;
}

} // namespace fox::csr3
```

- [ ] **Step 4: Add `Csr3_Command` to `src/main.cpp`** — mirror `Csr_Command` (main.cpp:776). Insert after `Csr2_Command` (~line 860). Add `#include "csr3/csr3.hpp"` after line 24.

```cpp
int Csr3_Command(Abc_Frame_t *pAbc, int argc, char **argv)
{
    fox::csr3::Config cfg;
    Abc_Ntk_t *pNtk = Abc_FrameReadNtk(pAbc);
    if (argc > 1 && !strcmp(argv[1], "-h"))
        goto usage;
    for (int i = 1; i != argc; ++i)
    {
        if (argv[i][0] != '-') { std::cout << "csr3: unexpected argument " << argv[i] << "\n"; goto usage; }
        const char arg = *(argv[i] + 1);
        switch (arg)
        {
        case 'J':
            if (i + 1 >= argc) { printf("csr3: -J requires a number\n"); return 1; }
            cfg.jaccard_pct = std::atoi(argv[++i]);
            if (cfg.jaccard_pct < 1 || cfg.jaccard_pct > 99) { printf("csr3: invalid -J %d\n", cfg.jaccard_pct); return 1; }
            break;
        case 'M':
            if (i + 1 >= argc) { printf("csr3: -M requires a number\n"); return 1; }
            cfg.max_lines = std::atoi(argv[++i]);
            if (cfg.max_lines < 1 || cfg.max_lines > 63) { printf("csr3: invalid -M %d (1-63)\n", cfg.max_lines); return 1; }
            break;
        case 'P':
            if (i + 1 >= argc) { printf("csr3: -P requires a number\n"); return 1; }
            cfg.sim_words = std::atoi(argv[++i]);
            if (cfg.sim_words < 1) { printf("csr3: invalid -P %d\n", cfg.sim_words); return 1; }
            break;
        case 'B':
            if (i + 1 >= argc) { printf("csr3: -B requires a number\n"); return 1; }
            cfg.btlimit = std::atoi(argv[++i]);
            if (cfg.btlimit < 1) { printf("csr3: invalid -B %d\n", cfg.btlimit); return 1; }
            break;
        case 'c': cfg.self_check ^= 1; break;
        case 'v': cfg.verbose ^= 1; break;
        case 'h': goto usage;
        default:  std::cout << "csr3: unknown argument -" << arg << "\n"; goto usage;
        }
    }
    return fox::csr3::RunCsr3(pNtk, cfg) ? 0 : 1;
usage:
    Abc_Print(-2, "usage: csr3 [-J num] [-M num] [-P num] [-B num] [-cv]\n");
    Abc_Print(-2, "\t        Phase 0: measure combinational SDC water in cross-partition interconnect (read-only)\n");
    Abc_Print(-2, "\t-J num : Jaccard grouping threshold, percent (1-99) [default = 30]\n");
    Abc_Print(-2, "\t-M num : max lines per group (bundle cap, 1-63) [default = 16]\n");
    Abc_Print(-2, "\t-P num : random simulation words (x64 patterns each) [default = 16]\n");
    Abc_Print(-2, "\t-B num : SAT backtrack limit per solve [default = 100000]\n");
    Abc_Print(-2, "\t-c     : self-check exhaustively for |support|<=16 groups\n");
    Abc_Print(-2, "\t-v     : toggle verbose (per-group detail)\n");
    return 1;
}
```

Register after main.cpp:1072:
```cpp
Cmd_CommandAdd(Abc_FrameGetGlobalFrame(), "FoxSYN", "csr3", Csr3_Command, 1);
```

- [ ] **Step 5: Wire `src/CMakeLists.txt`** — three edits.

After `add_subdirectory(csr2)` (line 48):
```cmake
add_subdirectory(csr3)
```
In `target_link_libraries(FoxSYN PRIVATE ...)`, after `csr2`:
```cmake
    csr3
```
After the `test_csr2` executable block (~line 109), add:
```cmake
add_executable(test_csr3 "test_csr3.cpp")
target_link_libraries(test_csr3 PRIVATE
    libabc
    timer
    csr3
)
```

- [ ] **Step 6: Write `src/test_csr3.cpp`** with one `ceil_log2` test (proves link + build).

```cpp
#include <cstdio>
#include "csr3/csr3.hpp"
#include "csr3/csr3_internal.hpp"

namespace {

int g_fail = 0;

void ExpectEqLong(const char *label, long actual, long expected)
{
    if (actual != expected) {
        std::fprintf(stderr, "FAIL %s: expected %ld, got %ld\n", label, expected, actual);
        ++g_fail;
    }
}

void TestCeilLog2()
{
    ExpectEqLong("ceil_log2(1)", fox::csr3::ceil_log2(1), 0);
    ExpectEqLong("ceil_log2(2)", fox::csr3::ceil_log2(2), 1);
    ExpectEqLong("ceil_log2(3)", fox::csr3::ceil_log2(3), 2);
    ExpectEqLong("ceil_log2(4)", fox::csr3::ceil_log2(4), 2);
    ExpectEqLong("ceil_log2(5)", fox::csr3::ceil_log2(5), 3);
    ExpectEqLong("ceil_log2(256)", fox::csr3::ceil_log2(256), 8);
}

} // namespace

int main()
{
    TestCeilLog2();
    if (g_fail == 0) std::printf("all csr3 tests passed\n");
    return g_fail == 0 ? 0 : 1;
}
```

- [ ] **Step 7: Build and run**

Run: `cd /home/longfei/FoxSYN && make 2>&1 | tail -20 && ./release/test_csr3`
Expected: build succeeds; prints `all csr3 tests passed`.

- [ ] **Step 8: Commit**

```bash
git add src/csr3 src/test_csr3.cpp src/CMakeLists.txt src/main.cpp
git commit -m "csr3: scaffold Phase 0 module, command, and build wiring"
```

---

### Task 2: collect_crossing_signals

**Files:**
- Modify: `src/csr3/csr3.cpp` (add function + includes `<vector>`)
- Test: `src/test_csr3.cpp`

**Interfaces:**
- Consumes: `Abc_ObjGetPartId`, `Abc_NtkForEachNode`, `Abc_ObjForEachFanout`.
- Produces: `std::vector<Abc_Obj_t*> collect_crossing_signals(Abc_Ntk_t*, int srcPart)`.

- [ ] **Step 1: Write the failing test** — build a 2-partition logic Ntk: PIs a,b in part 0; node n0 = a&b in part 0; node n1 = buf(n0) in part 1; PO from n1. Then n0 crosses 0→1. Add to `test_csr3.cpp`.

```cpp
#include <vector>
#include "base/abc/abc.h"
#include "base/abc/abcPdb.hpp"

// helper: give a node an AND SOP over its current fanins
static void SetAnd(Abc_Obj_t *n)
{
    auto *pMan = static_cast<Mem_Flex_t *>(n->pNtk->pManFunc);
    if (Abc_ObjFaninNum(n) == 1) n->pData = Abc_SopCreateBuf(pMan);
    else n->pData = Abc_SopCreateAnd(pMan, Abc_ObjFaninNum(n), nullptr);
}

void TestCollectCrossing()
{
    Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *b = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *n0 = Abc_NtkCreateNode(pNtk);
    Abc_ObjAddFanin(n0, a); Abc_ObjAddFanin(n0, b); SetAnd(n0);
    Abc_Obj_t *n1 = Abc_NtkCreateNode(pNtk);
    Abc_ObjAddFanin(n1, n0); SetAnd(n1);
    Abc_Obj_t *po = Abc_NtkCreatePo(pNtk);
    Abc_ObjAddFanin(po, n1);

    // partition: everything part 0 except n1 part 1
    Abc_ObjSetPartId(a, 0); Abc_ObjSetPartId(b, 0);
    Abc_ObjSetPartId(n0, 0); Abc_ObjSetPartId(n1, 1);
    Abc_NtkSetPartStats(pNtk, 2, 0, 0);

    auto cross01 = fox::csr3::collect_crossing_signals(pNtk, 0);
    ExpectEqLong("cross01 size", (long)cross01.size(), 1);
    ExpectEqLong("cross01 is n0", (long)(cross01.empty()?-1:cross01[0]->Id), (long)n0->Id);
    auto cross10 = fox::csr3::collect_crossing_signals(pNtk, 1);
    ExpectEqLong("cross10 size", (long)cross10.size(), 0);

    Abc_NtkDelete(pNtk);
}
```
Call `TestCollectCrossing();` in `main`.

- [ ] **Step 2: Run to verify it fails**

Run: `make 2>&1 | tail -20`
Expected: link error / `collect_crossing_signals` not defined.

- [ ] **Step 3: Implement** in `csr3.cpp`:

```cpp
std::vector<Abc_Obj_t*> collect_crossing_signals(Abc_Ntk_t *pNtk, int srcPart)
{
    std::vector<Abc_Obj_t*> out;
    Abc_Obj_t *pObj, *pFanout;
    int i, j;
    Abc_NtkForEachNode(pNtk, pObj, i)
    {
        if ((int)Abc_ObjGetPartId(pObj) != srcPart)
            continue;
        bool crosses = false;
        Abc_ObjForEachFanout(pObj, pFanout, j)
        {
            if (Abc_ObjIsNode(pFanout) &&
                Abc_PartIdIsValid(Abc_ObjGetPartId(pFanout)) &&
                (int)Abc_ObjGetPartId(pFanout) != srcPart)
            { crosses = true; break; }
        }
        if (crosses)
            out.push_back(pObj);
    }
    return out;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `make 2>&1 | tail -5 && ./release/test_csr3`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/csr3/csr3.cpp src/test_csr3.cpp
git commit -m "csr3: collect cross-partition crossing signals per direction"
```

---

### Task 3: extract_support_partition_aware + is_cone_leaf

**Files:**
- Modify: `src/csr3/csr3.cpp` (add `<algorithm>`, `<functional>`)
- Test: `src/test_csr3.cpp`

**Interfaces:**
- Consumes: `Abc_ObjIsCi`, `Abc_ObjIsNode`, `Abc_ObjFaninNum`, `Abc_ObjGetPartId`, TravId API.
- Produces: `is_cone_leaf(Abc_Obj_t*, int)`, `extract_support_partition_aware(Abc_Obj_t*, int) -> sorted unique leaf ObjIds`.

- [ ] **Step 1: Write the failing test** — reuse the Task 2 network; n0's cone in part 0 stops at PIs a,b → support {a.Id, b.Id}. Add a second network where an opposite-partition node feeds the line, asserting it becomes a leaf (not recursed through).

```cpp
void TestExtractSupport()
{
    Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *b = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *opp = Abc_NtkCreateNode(pNtk);       // opposite-partition feeder
    Abc_ObjAddFanin(opp, a); SetAnd(opp);
    Abc_Obj_t *n0 = Abc_NtkCreateNode(pNtk);
    Abc_ObjAddFanin(n0, b); Abc_ObjAddFanin(n0, opp); SetAnd(n0);
    Abc_Obj_t *po = Abc_NtkCreatePo(pNtk); Abc_ObjAddFanin(po, n0);

    Abc_ObjSetPartId(a, 0); Abc_ObjSetPartId(b, 0);
    Abc_ObjSetPartId(opp, 1);   // opposite partition => leaf boundary
    Abc_ObjSetPartId(n0, 0);
    Abc_NtkSetPartStats(pNtk, 2, 0, 0);

    auto supp = fox::csr3::extract_support_partition_aware(n0, 0);
    // support = { b (PI), opp (opposite-partition leaf) }; NOT a (behind opp)
    ExpectEqLong("supp size", (long)supp.size(), 2);
    bool hasB = false, hasOpp = false, hasA = false;
    for (int id : supp) { if (id==b->Id) hasB=true; if (id==opp->Id) hasOpp=true; if (id==a->Id) hasA=true; }
    ExpectEqLong("supp has b", hasB?1:0, 1);
    ExpectEqLong("supp has opp", hasOpp?1:0, 1);
    ExpectEqLong("supp excludes a (behind opp)", hasA?1:0, 0);

    Abc_NtkDelete(pNtk);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `make 2>&1 | tail -10`
Expected: undefined `extract_support_partition_aware`.

- [ ] **Step 3: Implement**

```cpp
bool is_cone_leaf(Abc_Obj_t *pObj, int srcPart)
{
    if (Abc_ObjIsCi(pObj))                                  // PI or FF-Q
        return true;
    if (Abc_ObjIsNode(pObj) && Abc_ObjFaninNum(pObj) == 0)  // constant node: keep as internal
        return false;
    if (Abc_ObjIsNode(pObj) && (int)Abc_ObjGetPartId(pObj) != srcPart)  // opposite-partition feeder
        return true;
    return false;                                           // same-partition node => internal
}

std::vector<int> extract_support_partition_aware(Abc_Obj_t *line, int srcPart)
{
    Abc_Ntk_t *pNtk = line->pNtk;
    std::vector<int> support;
    Abc_NtkIncrementTravId(pNtk);
    std::function<void(Abc_Obj_t*)> dfs = [&](Abc_Obj_t *pObj) {
        if (Abc_NodeIsTravIdCurrent(pObj)) return;
        Abc_NodeSetTravIdCurrent(pObj);
        if (is_cone_leaf(pObj, srcPart)) { support.push_back(pObj->Id); return; }
        Abc_Obj_t *pFanin; int i;
        Abc_ObjForEachFanin(pObj, pFanin, i) dfs(pFanin);
    };
    // the line driver itself is internal (same partition); walk its fanins
    Abc_Obj_t *pFanin; int i;
    Abc_NodeSetTravIdCurrent(line);
    Abc_ObjForEachFanin(line, pFanin, i) dfs(pFanin);
    std::sort(support.begin(), support.end());
    support.erase(std::unique(support.begin(), support.end()), support.end());
    return support;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `make 2>&1 | tail -5 && ./release/test_csr3`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/csr3/csr3.cpp src/test_csr3.cpp
git commit -m "csr3: partition-bounded support extraction (cone leaves as free vars)"
```

---

### Task 4: group_by_jaccard

**Files:**
- Modify: `src/csr3/csr3.cpp`
- Test: `src/test_csr3.cpp`

**Interfaces:**
- Consumes: `Line` (driver + sorted support).
- Produces: `group_by_jaccard(const std::vector<Line>&, int jaccardPct, int kmax) -> std::vector<Group>`.

Grouping: build an undirected graph over lines, edge if `100*|∩| / |∪| > jaccardPct`; take connected components; split any component larger than `kmax` into consecutive chunks of `kmax`. Lines with empty support go in singleton groups (no overlap possible).

- [ ] **Step 1: Write the failing test** — three lines: L0 supp {1,2,3}, L1 supp {2,3,4} (J=2/4=50%>30 ⇒ same group), L2 supp {10,11} (disjoint ⇒ own group). kmax=16.

```cpp
void TestGroupByJaccard()
{
    using fox::csr3::Line; using fox::csr3::Group;
    std::vector<Line> lines(3);
    lines[0].support = {1,2,3};
    lines[1].support = {2,3,4};
    lines[2].support = {10,11};
    auto groups = fox::csr3::group_by_jaccard(lines, 30, 16);
    ExpectEqLong("group count", (long)groups.size(), 2);
    // find the group with 2 lines
    int big = -1, small = -1;
    for (size_t i=0;i<groups.size();i++) {
        if (groups[i].lines.size()==2) big=(int)i;
        if (groups[i].lines.size()==1) small=(int)i;
    }
    ExpectEqLong("has 2-line group", big>=0?1:0, 1);
    ExpectEqLong("has 1-line group", small>=0?1:0, 1);
}
```
Note: `Line.driver` may be nullptr here; grouping only reads `support`, and `Group.lines` stores the drivers. Adapt: store index-based drivers — set `lines[i].driver = (Abc_Obj_t*)(intptr_t)(i+1)` sentinels so the test can distinguish, OR just assert sizes (chosen above: assert sizes only).

- [ ] **Step 2: Run to verify it fails**

Run: `make 2>&1 | tail -10`
Expected: undefined `group_by_jaccard`.

- [ ] **Step 3: Implement**

```cpp
static long intersize(const std::vector<int> &x, const std::vector<int> &y)
{
    long c = 0; size_t i = 0, j = 0;
    while (i < x.size() && j < y.size()) {
        if (x[i] == y[j]) { ++c; ++i; ++j; }
        else if (x[i] < y[j]) ++i; else ++j;
    }
    return c;
}

std::vector<Group> group_by_jaccard(const std::vector<Line> &lines, int jaccardPct, int kmax)
{
    int n = (int)lines.size();
    std::vector<int> comp(n, -1);
    // union-find
    std::vector<int> parent(n);
    for (int i = 0; i < n; ++i) parent[i] = i;
    std::function<int(int)> find = [&](int x){ while (parent[x]!=x){ parent[x]=parent[parent[x]]; x=parent[x]; } return x; };
    auto uni = [&](int a, int b){ parent[find(a)] = find(b); };

    for (int i = 0; i < n; ++i) {
        if (lines[i].support.empty()) continue;
        for (int j = i + 1; j < n; ++j) {
            if (lines[j].support.empty()) continue;
            long inter = intersize(lines[i].support, lines[j].support);
            if (inter == 0) continue;
            long uni_sz = (long)lines[i].support.size() + (long)lines[j].support.size() - inter;
            if (uni_sz > 0 && 100 * inter > (long)jaccardPct * uni_sz)
                uni(i, j);
        }
    }
    // bucket by root
    std::vector<std::vector<int>> buckets;
    std::vector<int> rootToBucket(n, -1);
    for (int i = 0; i < n; ++i) {
        int r = find(i);
        if (rootToBucket[r] == -1) { rootToBucket[r] = (int)buckets.size(); buckets.push_back({}); }
        buckets[rootToBucket[r]].push_back(i);
    }
    // emit, splitting oversized buckets into chunks of kmax
    std::vector<Group> groups;
    for (auto &b : buckets) {
        for (size_t s = 0; s < b.size(); s += (size_t)kmax) {
            Group g;
            for (size_t t = s; t < b.size() && t < s + (size_t)kmax; ++t)
                g.lines.push_back(lines[b[t]].driver);
            groups.push_back(std::move(g));
        }
    }
    return groups;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `make 2>&1 | tail -5 && ./release/test_csr3`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/csr3/csr3.cpp src/test_csr3.cpp
git commit -m "csr3: group crossing lines by support-overlap (Jaccard union-find)"
```

---

### Task 5: build_group_cone_ntk

Builds a standalone logic sub-network: one PI per union-support leaf, the group's k lines as POs, all same-partition internal cone nodes duplicated. This network is the input to both simulation (Task 6) and SAT (Task 7).

**Files:**
- Modify: `src/csr3/csr3.cpp`
- Test: `src/test_csr3.cpp`

**Interfaces:**
- Consumes: `Abc_NtkCleanCopy`, `Abc_NtkAlloc`, `Abc_NtkCreatePi/Po`, `Abc_NtkDupObj`, `Abc_ObjAddFanin`, `is_cone_leaf`.
- Produces: `build_group_cone_ntk(const std::vector<Abc_Obj_t*>&, int srcPart) -> Abc_Ntk_t*` (caller owns; `Abc_NtkDelete`).

- [ ] **Step 1: Write the failing test** — Task 2 network, group {n0}, srcPart 0. Cone Ntk should have 2 PIs (a,b), 1 PO, and be a valid logic network.

```cpp
void TestBuildConeNtk()
{
    Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *b = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *n0 = Abc_NtkCreateNode(pNtk);
    Abc_ObjAddFanin(n0, a); Abc_ObjAddFanin(n0, b); SetAnd(n0);
    Abc_Obj_t *po = Abc_NtkCreatePo(pNtk); Abc_ObjAddFanin(po, n0);
    Abc_ObjSetPartId(a,0); Abc_ObjSetPartId(b,0); Abc_ObjSetPartId(n0,0);
    Abc_NtkSetPartStats(pNtk, 2, 0, 0);

    std::vector<Abc_Obj_t*> grp = { n0 };
    Abc_Ntk_t *pCone = fox::csr3::build_group_cone_ntk(grp, 0);
    ExpectEqLong("cone PIs", (long)Abc_NtkPiNum(pCone), 2);
    ExpectEqLong("cone POs", (long)Abc_NtkPoNum(pCone), 1);
    ExpectEqLong("cone valid", Abc_NtkCheck(pCone)?1:0, 1);
    Abc_NtkDelete(pCone);
    Abc_NtkDelete(pNtk);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `make 2>&1 | tail -10`
Expected: undefined `build_group_cone_ntk`.

- [ ] **Step 3: Implement** (mirrors the logic branch of `Abc_NtkCreateConeArray`, abcNtk.c:1002, but with partition-bounded DFS)

```cpp
static void cone_build_dfs(Abc_Obj_t *pObj, Abc_Ntk_t *pCone, int srcPart)
{
    if (Abc_NodeIsTravIdCurrent(pObj)) return;
    Abc_NodeSetTravIdCurrent(pObj);
    if (is_cone_leaf(pObj, srcPart)) {
        pObj->pCopy = Abc_NtkCreatePi(pCone);
        return;
    }
    Abc_Obj_t *pFanin; int i;
    Abc_ObjForEachFanin(pObj, pFanin, i) cone_build_dfs(pFanin, pCone, srcPart);
    Abc_NtkDupObj(pCone, pObj, 0);              // copies SOP pData for logic nodes
    Abc_ObjForEachFanin(pObj, pFanin, i) Abc_ObjAddFanin(pObj->pCopy, pFanin->pCopy);
}

Abc_Ntk_t *build_group_cone_ntk(const std::vector<Abc_Obj_t*> &lines, int srcPart)
{
    Abc_Ntk_t *pNtk = lines[0]->pNtk;
    Abc_NtkCleanCopy(pNtk);
    Abc_Ntk_t *pCone = Abc_NtkAlloc(pNtk->ntkType, pNtk->ntkFunc, 1);
    pCone->pName = Extra_UtilStrsav("csr3_cone");
    Abc_NtkIncrementTravId(pNtk);
    for (Abc_Obj_t *line : lines) cone_build_dfs(line, pCone, srcPart);
    for (Abc_Obj_t *line : lines) {
        Abc_Obj_t *pPo = Abc_NtkCreatePo(pCone);
        Abc_ObjAddFanin(pPo, line->pCopy);
    }
    if (!Abc_NtkCheck(pCone))
        printf("csr3: warning: cone network check failed\n");
    return pCone;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `make 2>&1 | tail -5 && ./release/test_csr3`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/csr3/csr3.cpp src/test_csr3.cpp
git commit -m "csr3: build per-group cone sub-network (leaves as PIs, lines as POs)"
```

---

### Task 6: simulate_prefilter (bit-parallel) + count_m_exhaustive

`simulate_prefilter` gives a fast lower bound on m via random sim (used to drop no-water groups). `count_m_exhaustive` computes exact m by enumerating all `2^support` inputs (only when support ≤ 16); used by the `-c` self-check and by SAT verification. Both share a GIA-based bit-parallel evaluator.

**Files:**
- Modify: `src/csr3/csr3.cpp` (add `extern "C"` block for AIG/GIA headers + `<unordered_set>`)
- Test: `src/test_csr3.cpp`

**Interfaces:**
- Consumes: `Abc_NtkStrash`, `Abc_NtkToDar`, `Gia_ManFromAig`, `Gia_ManSimPatSim`, `Vec_Wrd`, `Gia_ManRandomW`, `Vec_WrdStartTruthTables`.
- Produces: `simulate_prefilter(Abc_Ntk_t* pCone, int k, int nWords) -> long` (distinct lower bound, capped at `(1<<k)`), `count_m_exhaustive(Abc_Ntk_t* pCone, int k) -> long`.

`extern "C"` header block to add near the top of `csr3.cpp`:
```cpp
extern "C" {
#include "aig/aig/aig.h"
#include "aig/gia/gia.h"
#include "sat/cnf/cnf.h"
#include "sat/bsat/satSolver.h"
Aig_Man_t * Abc_NtkToDar(Abc_Ntk_t *pNtk, int fExors, int fRegisters);
}
```

- [ ] **Step 1: Write the failing test** — cone where the two lines are IDENTICAL (both = a&b): only outputs (0,0) and (1,1) reachable ⇒ m=2, not 4. Exhaustive must return 2; sim lower bound must be ≤ 2 and ≥ 1.

```cpp
void TestSimAndExhaustive()
{
    Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *b = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *n0 = Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(n0,a); Abc_ObjAddFanin(n0,b); SetAnd(n0);
    Abc_Obj_t *n1 = Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(n1,a); Abc_ObjAddFanin(n1,b); SetAnd(n1);
    Abc_Obj_t *p0=Abc_NtkCreatePo(pNtk); Abc_ObjAddFanin(p0,n0);
    Abc_Obj_t *p1=Abc_NtkCreatePo(pNtk); Abc_ObjAddFanin(p1,n1);
    for (Abc_Obj_t*o : {a,b,n0,n1}) Abc_ObjSetPartId(o,0);
    Abc_NtkSetPartStats(pNtk, 2, 0, 0);

    std::vector<Abc_Obj_t*> grp = { n0, n1 };
    Abc_Ntk_t *pCone = fox::csr3::build_group_cone_ntk(grp, 0);
    long mEx = fox::csr3::count_m_exhaustive(pCone, 2);
    ExpectEqLong("exhaustive m (identical lines)", mEx, 2);
    long mSim = fox::csr3::simulate_prefilter(pCone, 2, 4);
    ExpectEqLong("sim lb <= m", (mSim <= 2)?1:0, 1);
    ExpectEqLong("sim lb >= 1", (mSim >= 1)?1:0, 1);
    Abc_NtkDelete(pCone); Abc_NtkDelete(pNtk);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `make 2>&1 | tail -10`
Expected: undefined `count_m_exhaustive` / `simulate_prefilter`.

- [ ] **Step 3: Implement**

```cpp
// Assemble the k-bit output tuple for pattern p (0..totalPats-1) from GIA CO sim words.
// Returns tuples counted into a hash set; k <= 63 guaranteed by -M cap.
static long count_distinct_from_sim(Gia_Man_t *pGia, Vec_Wrd_t *vSims, int k, int nWords)
{
    std::unordered_set<uint64_t> seen;
    int totalPats = nWords * 64;
    for (int p = 0; p < totalPats; ++p) {
        int w = p >> 6, bit = p & 63;
        uint64_t key = 0;
        for (int j = 0; j < k; ++j) {
            Gia_Obj_t *pCo = Gia_ManCo(pGia, j);
            word *co = Vec_WrdArray(vSims) + (long)nWords * Gia_ObjId(pGia, pCo);
            uint64_t v = (co[w] >> bit) & 1;
            key |= (v << j);
        }
        seen.insert(key);
    }
    return (long)seen.size();
}

long simulate_prefilter(Abc_Ntk_t *pCone, int k, int nWords)
{
    Abc_Ntk_t *pStrash = Abc_NtkStrash(pCone, 0, 1, 0);
    Aig_Man_t *pAig = Abc_NtkToDar(pStrash, 0, 0);
    Gia_Man_t *pGia = Gia_ManFromAig(pAig);
    int nCi = Gia_ManCiNum(pGia);
    Gia_ManRandomW(1);                                  // reset RNG for determinism
    pGia->vSimsPi = Vec_WrdAlloc((long)nWords * nCi);
    for (long i = 0; i < (long)nWords * nCi; ++i)
        Vec_WrdPush(pGia->vSimsPi, Gia_ManRandomW(0));
    Vec_Wrd_t *vSims = Gia_ManSimPatSim(pGia);
    long distinct = count_distinct_from_sim(pGia, vSims, k, nWords);
    Vec_WrdFree(vSims);
    Gia_ManStop(pGia);
    Aig_ManStop(pAig);
    Abc_NtkDelete(pStrash);
    return distinct;
}

long count_m_exhaustive(Abc_Ntk_t *pCone, int k)
{
    Abc_Ntk_t *pStrash = Abc_NtkStrash(pCone, 0, 1, 0);
    Aig_Man_t *pAig = Abc_NtkToDar(pStrash, 0, 0);
    Gia_Man_t *pGia = Gia_ManFromAig(pAig);
    int nCi = Gia_ManCiNum(pGia);
    // exhaustive: 2^nCi input patterns via canonical truth-table columns
    // Vec_WrdStartTruthTables(nCi) lays out nCi vars over 2^nCi patterns (nWords = max(1, 2^(nCi-6)))
    int nWords = nCi <= 6 ? 1 : (1 << (nCi - 6));
    pGia->vSimsPi = Vec_WrdStartTruthTables(nCi);       // exactly enumerates all 2^nCi inputs
    Vec_Wrd_t *vSims = Gia_ManSimPatSim(pGia);
    long m = count_distinct_from_sim(pGia, vSims, k, nWords);
    Vec_WrdFree(vSims);
    Gia_ManStop(pGia);
    Aig_ManStop(pAig);
    Abc_NtkDelete(pStrash);
    return m;
}
```

Note: `count_m_exhaustive` is valid only for `nCi <= 16` (2^16 patterns = 1024 words). Callers must gate on support size before calling.

- [ ] **Step 4: Run to verify it passes**

Run: `make 2>&1 | tail -5 && ./release/test_csr3`
Expected: PASS (exhaustive m=2, sim within [1,2]).

- [ ] **Step 5: Commit**

```bash
git add src/csr3/csr3.cpp src/test_csr3.cpp
git commit -m "csr3: bit-parallel simulation prefilter and exhaustive m counter"
```

---

### Task 7: count_m_sat (All-SAT with projection + blocking clauses)

Exact reachable-combination count via SAT projected onto the k output driver vars. Verified against `count_m_exhaustive` on small cones (this is the design's safety net for the CO-var-indexing subtlety).

**Files:**
- Modify: `src/csr3/csr3.cpp`
- Test: `src/test_csr3.cpp`

**Interfaces:**
- Consumes: `Abc_NtkStrash`, `Abc_NtkToDar`, `Cnf_Derive`, `Cnf_DataWriteIntoSolver`, `sat_solver_solve`, `sat_solver_addclause`, `sat_solver_var_value`, `Abc_Var2Lit`, `Aig_ManCo`, `Aig_ObjFanin0`, `pCnf->pVarNums`.
- Produces: `count_m_sat(Abc_Ntk_t* pCone, int k, int btlimit) -> long` — exact m; returns `(1<<k)` sentinel when count exceeds `(1<<(k-1))` (early exit = no water); `-1` on timeout.

Key correctness fact (design spec §Step 5): output_j = driver_var_j XOR complement_j is a per-line bijection, so counting distinct assignments to the driver vars equals counting distinct output tuples. Blocking is done over those vars; complements are irrelevant to the count. Constant-driven lines contribute no variable and are skipped from the projection.

- [ ] **Step 1: Write the failing test** — reuse the identical-lines cone (m=2) and a disjoint cone (two lines over disjoint inputs ⇒ m=4). Also cross-check against exhaustive.

```cpp
void TestCountMSat()
{
    // identical lines => m=2
    {
        Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
        Abc_Obj_t *a=Abc_NtkCreatePi(pNtk), *b=Abc_NtkCreatePi(pNtk);
        Abc_Obj_t *n0=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(n0,a);Abc_ObjAddFanin(n0,b);SetAnd(n0);
        Abc_Obj_t *n1=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(n1,a);Abc_ObjAddFanin(n1,b);SetAnd(n1);
        Abc_Obj_t *p0=Abc_NtkCreatePo(pNtk);Abc_ObjAddFanin(p0,n0);
        Abc_Obj_t *p1=Abc_NtkCreatePo(pNtk);Abc_ObjAddFanin(p1,n1);
        for (Abc_Obj_t*o:{a,b,n0,n1}) Abc_ObjSetPartId(o,0);
        Abc_NtkSetPartStats(pNtk,2,0,0);
        std::vector<Abc_Obj_t*> grp={n0,n1};
        Abc_Ntk_t *pCone=fox::csr3::build_group_cone_ntk(grp,0);
        ExpectEqLong("sat m identical", fox::csr3::count_m_sat(pCone,2,100000), 2);
        ExpectEqLong("sat==exhaustive identical",
            fox::csr3::count_m_sat(pCone,2,100000)==fox::csr3::count_m_exhaustive(pCone,2)?1:0, 1);
        Abc_NtkDelete(pCone); Abc_NtkDelete(pNtk);
    }
    // disjoint lines => m=4
    {
        Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
        Abc_Obj_t *a=Abc_NtkCreatePi(pNtk),*b=Abc_NtkCreatePi(pNtk),*c=Abc_NtkCreatePi(pNtk),*d=Abc_NtkCreatePi(pNtk);
        Abc_Obj_t *n0=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(n0,a);Abc_ObjAddFanin(n0,b);SetAnd(n0);
        Abc_Obj_t *n1=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(n1,c);Abc_ObjAddFanin(n1,d);SetAnd(n1);
        Abc_Obj_t *p0=Abc_NtkCreatePo(pNtk);Abc_ObjAddFanin(p0,n0);
        Abc_Obj_t *p1=Abc_NtkCreatePo(pNtk);Abc_ObjAddFanin(p1,n1);
        for (Abc_Obj_t*o:{a,b,c,d,n0,n1}) Abc_ObjSetPartId(o,0);
        Abc_NtkSetPartStats(pNtk,2,0,0);
        std::vector<Abc_Obj_t*> grp={n0,n1};
        Abc_Ntk_t *pCone=fox::csr3::build_group_cone_ntk(grp,0);
        ExpectEqLong("sat m disjoint", fox::csr3::count_m_sat(pCone,2,100000), 4);
        Abc_NtkDelete(pCone); Abc_NtkDelete(pNtk);
    }
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `make 2>&1 | tail -10`
Expected: undefined `count_m_sat`.

- [ ] **Step 3: Implement** (All-SAT loop modeled on `sat/bmc/bmcClp.c`; `l_False == -1` per `sat/bsat/satSolver.h`)

```cpp
long count_m_sat(Abc_Ntk_t *pCone, int k, int btlimit)
{
    Abc_Ntk_t *pStrash = Abc_NtkStrash(pCone, 0, 1, 0);
    Aig_Man_t *pAig = Abc_NtkToDar(pStrash, 0, 0);
    Cnf_Dat_t *pCnf = Cnf_Derive(pAig, Aig_ManCoNum(pAig));
    sat_solver *pSat = (sat_solver *)Cnf_DataWriteIntoSolver(pCnf, 1, 0);

    // projection vars: the CNF var of each line's driver (skip constant drivers)
    std::vector<int> outVars;
    for (int j = 0; j < k; ++j) {
        Aig_Obj_t *pCo = Aig_ManCo(pAig, j);
        Aig_Obj_t *pDrv = Aig_ObjFanin0(pCo);
        if (pDrv == NULL || Aig_ObjIsConst1(pDrv)) continue;   // constant line: no variability
        int var = pCnf->pVarNums[Aig_ObjId(pDrv)];
        if (var >= 0) outVars.push_back(var);
    }

    long cap = (k >= 1) ? (1L << (k - 1)) : 1;   // early-exit threshold: > 2^(k-1) => no water
    long m = 0;
    std::vector<int> lits;
    long result;
    if (pSat == NULL) {                          // trivially UNSAT constant network
        m = 1; goto done;
    }
    while (true) {
        int status = sat_solver_solve(pSat, NULL, NULL, (ABC_INT64_T)btlimit, 0, 0, 0);
        if (status == -1) break;                 // l_False: enumerated all
        if (status == 0)  { m = -1; goto cleanup; } // l_Undef: timeout
        ++m;
        if (m > cap) { m = (1L << k); goto cleanup; } // early exit: gain would be 0
        // block this assignment over the projection vars
        lits.clear();
        for (int var : outVars) {
            int val = sat_solver_var_value(pSat, var);
            lits.push_back(Abc_Var2Lit(var, val)); // literal true when var != val
        }
        if (lits.empty()) break;                 // all lines constant => single combination
        if (!sat_solver_addclause(pSat, lits.data(), lits.data() + lits.size()))
            break;                               // adding blocking clause made it UNSAT
    }
    goto done;
cleanup:
done:
    if (pSat) sat_solver_delete(pSat);
    Cnf_DataFree(pCnf);
    Aig_ManStop(pAig);
    Abc_NtkDelete(pStrash);
    if (m < 1 && m != -1) m = 1;
    return m;
}
```

Note on the `lits.empty()` case: if every line is constant, there is exactly one reachable combination ⇒ m=1. The loop breaks after the first model with no blocking clause added; `m` is 1. Verify this against `count_m_exhaustive` when a constant-line test is added.

- [ ] **Step 4: Run to verify it passes**

Run: `make 2>&1 | tail -5 && ./release/test_csr3`
Expected: PASS (m=2 identical, m=4 disjoint, sat==exhaustive).

- [ ] **Step 5: Commit**

```bash
git add src/csr3/csr3.cpp src/test_csr3.cpp
git commit -m "csr3: exact All-SAT reachable-combination counter with early exit"
```

---

### Task 8: Orchestration, reporting, and real experiment

Wire the units into `RunCsr3`: for each direction, collect → extract support → group → per group (build cone → prefilter → SAT count or exhaustive `-c`) → aggregate → report. Then run on real benchmarks — the deliverable.

**Files:**
- Modify: `src/csr3/csr3.cpp` (replace stub `RunCsr3` body)
- Test: `src/test_csr3.cpp` (end-to-end on a hand-built 2-partition net)

**Interfaces:**
- Consumes: every unit function above.
- Produces: final `RunCsr3` behavior + printed report.

- [ ] **Step 1: Write the failing end-to-end test** — a net with a known cross-partition redundancy (two identical crossing signals 0→1) should report gain ≥ 1 in direction 0→1.

```cpp
void TestEndToEnd()
{
    Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a=Abc_NtkCreatePi(pNtk), *b=Abc_NtkCreatePi(pNtk);
    // two identical part-0 nodes, both consumed in part 1 => a redundant crossing pair
    Abc_Obj_t *n0=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(n0,a);Abc_ObjAddFanin(n0,b);SetAnd(n0);
    Abc_Obj_t *n1=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(n1,a);Abc_ObjAddFanin(n1,b);SetAnd(n1);
    Abc_Obj_t *s=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(s,n0);Abc_ObjAddFanin(s,n1);SetAnd(s); // part 1 sink
    Abc_Obj_t *po=Abc_NtkCreatePo(pNtk); Abc_ObjAddFanin(po,s);
    Abc_ObjSetPartId(a,0);Abc_ObjSetPartId(b,0);Abc_ObjSetPartId(n0,0);Abc_ObjSetPartId(n1,0);
    Abc_ObjSetPartId(s,1);
    Abc_NtkSetPartStats(pNtk,2,0,0);

    fox::csr3::Config cfg; cfg.self_check = true;
    bool ok = fox::csr3::RunCsr3(pNtk, cfg);
    ExpectEqLong("RunCsr3 ok", ok?1:0, 1);
    // network unchanged (read-only): still 2 PIs, 1 PO, 3 nodes
    ExpectEqLong("nodes unchanged", (long)Abc_NtkNodeNum(pNtk), 3);
    Abc_NtkDelete(pNtk);
}
```
(This test asserts the run completes and does not mutate the network. Numeric gain is printed for manual inspection.)

- [ ] **Step 2: Run to verify it fails**

Run: `make 2>&1 | tail -10 && ./release/test_csr3`
Expected: FAIL — stub prints "not yet implemented", but network-unchanged check may pass; the run should still return true. If the stub returns true and doesn't mutate, this test passes trivially — so ALSO assert a report side-effect: capture that `RunCsr3` prints a line containing "recoverable". Simplest: skip output capture; instead this task's real proof is the benchmark run in Step 4. Keep the test as a read-only + completes smoke test.

- [ ] **Step 3: Implement the full `RunCsr3`** (replace the stub body from Task 1)

```cpp
bool RunCsr3(Abc_Ntk_t *pNtk, const Config &cfg)
{
    if (!pNtk) { printf("csr3: current network is empty\n"); return false; }
    if (!Abc_NtkIsLogic(pNtk)) { printf("csr3: network must be logic (not AIG)\n"); return false; }
    if (!pNtk->pPdb) { printf("csr3: no partition database (run hpart first)\n"); return false; }
    if (Abc_NtkPdb(pNtk)->num_parts() != 2) {
        printf("csr3: v1 only supports N=2 partitions (got %d)\n", Abc_NtkPdb(pNtk)->num_parts());
        return false;
    }

    long globalGain = 0, globalK = 0;
    for (int srcPart = 0; srcPart <= 1; ++srcPart) {
        int dstPart = 1 - srcPart;
        std::vector<Abc_Obj_t*> crossing = collect_crossing_signals(pNtk, srcPart);
        std::vector<Line> lines;
        lines.reserve(crossing.size());
        for (Abc_Obj_t *drv : crossing) {
            Line ln; ln.driver = drv;
            ln.support = extract_support_partition_aware(drv, srcPart);
            lines.push_back(std::move(ln));
        }
        std::vector<Group> groups = group_by_jaccard(lines, cfg.jaccard_pct, cfg.max_lines);

        long dirGain = 0, dirK = 0;
        int nPrefiltered = 0;
        // support-size lookup for self-check gating
        // (rebuild union support size per group via the cone's PI count)
        for (Group &g : groups) {
            int k = (int)g.lines.size();
            if (k == 0) continue;
            Abc_Ntk_t *pCone = build_group_cone_ntk(g.lines, srcPart);
            int suppSize = Abc_NtkPiNum(pCone);
            long simLb = simulate_prefilter(pCone, k, cfg.sim_words);
            long m;
            bool prefiltered = false;
            if (simLb > (1L << (k - 1))) {   // simulation already proves no water
                m = (1L << k);
                prefiltered = true;
                ++nPrefiltered;
            } else {
                m = count_m_sat(pCone, k, cfg.btlimit);
                if (m == -1) { m = (1L << k); }   // timeout: treat as no water (conservative)
                if (cfg.self_check && suppSize <= 16) {
                    long mEx = count_m_exhaustive(pCone, k);
                    if (m != mEx)
                        printf("csr3: SELF-CHECK MISMATCH dir %d->%d: sat m=%ld exhaustive m=%ld (k=%d supp=%d)\n",
                               srcPart, dstPart, m, mEx, k, suppSize);
                }
            }
            int gain = k - ceil_log2(m);
            if (gain < 0) gain = 0;
            dirGain += gain; dirK += k;
            if (cfg.verbose)
                printf("  [%d->%d] group k=%d supp=%d m=%ld gain=%d%s\n",
                       srcPart, dstPart, k, suppSize, m, gain, prefiltered ? " (sim-pruned)" : "");
            Abc_NtkDelete(pCone);
        }
        globalGain += dirGain; globalK += dirK;
        printf("csr3: dir %d->%d: %d crossing signals, %zu groups (%d sim-pruned), "
               "sum-k=%ld recoverable=%ld\n",
               srcPart, dstPart, (int)crossing.size(), groups.size(), nPrefiltered, dirK, dirGain);
    }

    double pct = globalK > 0 ? 100.0 * (double)globalGain / (double)globalK : 0.0;
    printf("csr3: TOTAL recoverable wires (detected-floor, combinational SDC only) = %ld / %ld crossing (%.1f%%)\n",
           globalGain, globalK, pct);
    printf("csr3: NOTE this is a lower bound; reachability/ODC water (e.g. one-hot buses) is NOT measured.\n");
    return true;
}
```

- [ ] **Step 4: Run the test and a real benchmark**

Run: `make 2>&1 | tail -5 && ./release/test_csr3`
Expected: PASS; per-group verbose lines show gain≥1 for the identical-pair group; no SELF-CHECK MISMATCH.

Then a real circuit (find one under `regression/`):
```bash
ls regression | head
./release/FoxSYN -c "read regression/<some>.v; st; if -K 6; hpart -N 2; csr3 -v -c; quit"
```
Expected: partition runs, csr3 prints per-direction and TOTAL lines, zero SELF-CHECK MISMATCH. Record the TOTAL recoverable percentage — this is the deliverable datum.

- [ ] **Step 5: Commit**

```bash
git add src/csr3/csr3.cpp src/test_csr3.cpp
git commit -m "csr3: orchestrate Phase 0 measurement and emit detected-floor report"
```

---

### Task 9: Regression sweep script (optional, produces the decision data)

A small script to run `csr3` across the benchmark suite and tabulate recoverable percentages, mirroring `scripts/run_csr_regression.py`.

**Files:**
- Create: `scripts/run_csr3_measure.py`

**Interfaces:**
- Consumes: the `FoxSYN` binary + `csr3` command.
- Produces: a CSV/table of `benchmark, cross_0to1, cross_1to0, recoverable, pct`.

- [ ] **Step 1: Read `scripts/run_csr_regression.py`** to match invocation/CEC conventions and benchmark discovery.

Run: `sed -n '1,60p' scripts/run_csr_regression.py`

- [ ] **Step 2: Write `scripts/run_csr3_measure.py`** — for each benchmark: run `read; st; if -K 6; hpart -N 2; csr3` (no `-v`), parse the `TOTAL recoverable` line, collect into a table. No CEC needed (read-only pass). Follow the existing script's subprocess + argument style; do not invent new frameworks. (Full script content to be written matching the conventions found in Step 1.)

- [ ] **Step 3: Run the sweep**

Run: `python3 scripts/run_csr3_measure.py --limit 20`
Expected: table of recoverable percentages across benchmarks. This answers "does the interconnect have water" with data.

- [ ] **Step 4: Commit**

```bash
git add scripts/run_csr3_measure.py
git commit -m "csr3: add Phase 0 measurement sweep script"
```

---

## Self-Review

**1. Spec coverage** (against `docs/csr3-phase0-design.md`):
- §2 束 model (two directions, cut-net unit) → Task 2 (`collect_crossing_signals`, node-fanout check, one entry per driver). ✓
- §3 Step 2 partition-aware cone → Task 3 (`is_cone_leaf` stops at PI/FF/const-internal/opposite-partition). ✓
- §3 Step 3 Jaccard grouping → Task 4. ✓
- §3 Step 4 simulation prefilter (lower bound, drop >2^(k-1)) → Task 6 + Task 8 gating. ✓
- §3 Step 5 SAT All-SAT exact m with early exit → Task 7. ✓
- §3 Step 7 report (per-dir, global, detected-floor wording) → Task 8. ✓
- §4 command interface (-J/-M/-P/-B/-c/-v, four guards) → Task 1. ✓
- §5 file structure + build wiring → Task 1. ✓
- §6.1 self-check `-c` → Task 6 (`count_m_exhaustive`) + Task 8 (mismatch assert). ✓
- §6.2 invariants (m≤2^k, gain≥0, sim≤m) → enforced in Task 8 (`gain<0→0`) and cross-checked by tests. ✓
- §6.3 real experiment → Task 8 Step 4 + Task 9. ✓

**2. Placeholder scan:** Task 9 Step 2 says "full script content to be written matching conventions found in Step 1" — this is intentional (the script mirrors an existing file the worker reads in Step 1); Task 9 is marked optional. All code-bearing steps in Tasks 1-8 contain complete code. No TBD/TODO in Tasks 1-8.

**3. Type consistency:** `Line`/`Group`/`GroupResult`/`Config` defined once in the Interfaces section; `count_m_sat`/`count_m_exhaustive`/`simulate_prefilter`/`build_group_cone_ntk`/`group_by_jaccard`/`extract_support_partition_aware`/`collect_crossing_signals`/`is_cone_leaf`/`ceil_log2` names identical across declaration and all call sites. `count_m_sat` early-exit sentinel `(1<<k)` matches Task 8's handling. `srcPart` is `int` throughout; compared to `part_id` via explicit `(int)` cast. ✓

**Known risk flagged for the implementer:** the CO-var projection in Task 7 (reading `pCnf->pVarNums[Aig_ObjId(Aig_ObjFanin0(pCo))]`) is the single subtlest piece. Task 7's test cross-checks `count_m_sat` against `count_m_exhaustive`, and Task 8's `-c` does so on every real group with support ≤ 16 — if the indexing is wrong, the self-check mismatch prints immediately. Do not skip the `-c` run in Task 8 Step 4.
