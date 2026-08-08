# hive Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the `hive` command (spec: `docs/hive-design.md`): a read-only measurement command that finds Top-N convex "narrow-interface, dense-interior" regions in a logic network and prints a report.

**Architecture:** Four layers, riskiest isolated: `CombGraph` (immutable combinational-graph snapshot with its own strictly-increasing rank), `Region` (small-container incremental in/out/N/Q/LB bookkeeping, cheap to copy for trial moves), `ComputeClosure` (rank-band violator search with budget; plus an independent brute-force checker used only by tests), and the growth pipeline in `hive.cpp` (MFFC seeds → greedy grow with best-prefix → Jaccard dedup → Top-N → report). Command wrapper in `main.cpp`.

**Tech Stack:** C++23, Berkeley ABC (`Abc_Ntk_t`, logic networks), CMake, hand-rolled test binary (`ExpectEq`/`ExpectTrue` style per `src/test_fmpart.cpp`).

## Global Constraints

- Spec is `docs/hive-design.md`; section references (§) below point there.
- Defaults verbatim (§6): `-N 20`, `-M 64`, `-I 32`, `-O 8`, `-K 6`, `-S 2000`. Ranges: `-N/-M/-I/-O ≥ 1`, `-K` in 2..16, `-S ≥ 0`.
- Internal constants (§6): `kTopL = 4`, `kStall = 8`, `kJaccardPct = 50`, `kClosureBudget = 2048`, `kFanoutCap = 16`. The last two values are fixed by this plan (spec left them open as "internal constants").
- Read-only (§6): never modify network structure or `pData`, never touch `Pdb`, **never call `Abc_NtkLevel`** — rank lives in `CombGraph`'s own arrays. Allowed side effects: travId and the paired `vFanouts.nSize` deref/ref inside `Abc_NodeMffcLabel`.
- const1 is an ordinary node (§2.1) — no special-case branch anywhere.
- Every reported region must be convex; convexity is verified in tests by an independent brute-force path (§8).
- Style: `src/hive/*` uses 4-space indent, braces on their own line (match `src/pdecomp/pdecomp.cpp`); `src/main.cpp` edits match that file's same-line brace style.
- Namespace `fox::hive`. Library links `libabc` only.
- Commit after every task, subject prefix `hive: `.

---

## File map

- Create `src/hive/hive.hpp` — `Config`, internal constants, `RegionReport`, `Result`, `RunHive`, `ApplyHive`.
- Create `src/hive/hive_internal.hpp` — `GrowResult`, `GrowFromSeed` (test exposure; precedent: `csr3_internal.hpp`).
- Create `src/hive/hive_graph.{hpp,cpp}` — `CombGraph`.
- Create `src/hive/region.{hpp,cpp}` — `Region`, `Metrics`.
- Create `src/hive/convex.{hpp,cpp}` — `ComputeClosure`, `IsConvexBrute`.
- Create `src/hive/hive.cpp` — seeds, growth, dedup, Top-N, report, `ApplyHive`.
- Create `src/hive/CMakeLists.txt`.
- Create `src/test_hive.cpp`.
- Modify `src/CMakeLists.txt` — `add_subdirectory(hive)`, link `hive` into `FoxSYN`, add `test_hive`.
- Modify `src/main.cpp` — include, `Hive_Command`, registration.
- Create `docs/hive.md` — user-facing command doc (repo convention, cf. `docs/hpart.md`).

---

### Task 1: Scaffolding — public header, stubs, build wiring, test harness

**Files:**
- Create: `src/hive/hive.hpp`, `src/hive/CMakeLists.txt`, `src/hive/hive.cpp` (stub)
- Modify: `src/CMakeLists.txt`
- Create: `src/test_hive.cpp`

**Interfaces:**
- Produces: `fox::hive::Config` (fields `num_regions=20, max_nodes=64, max_in=32, max_out=8, lut_k=6, num_seeds=2000, verbose=false`), constants `kTopL=4, kStall=8, kJaccardPct=50, kClosureBudget=2048, kFanoutCap=16`, `RegionReport`, `Result`, `Result RunHive(Abc_Ntk_t*, const Config&)`, `bool ApplyHive(Abc_Frame_t*, const Config&)`. Consumed by every later task.

- [ ] **Step 1: Write `src/hive/hive.hpp`**

```cpp
#ifndef HIVE_HPP
#define HIVE_HPP

#include <vector>

#include "misc/util/abc_global.h"
#include "base/main/main.h"

namespace fox::hive {

struct Config {
    int num_regions = 20;   // -N: regions to report (>=1)
    int max_nodes   = 64;   // -M: region node cap (>=1)
    int max_in      = 32;   // -I: input cap Imax (>=1)
    int max_out     = 8;    // -O: output cap Omax (>=1)
    int lut_k       = 6;    // -K: LUT width, LB computation only (2-16)
    int num_seeds   = 2000; // -S: seed cap, 0 = all nodes (>=0)
    bool verbose    = false;// -v
};

// Internal constants (docs/hive-design.md 6). Exposed so tests can reference
// the exact values they pin as regression baselines.
inline constexpr int kTopL          = 4;    // exact evaluations per growth step
inline constexpr int kStall         = 8;    // steps without a new best before stop
inline constexpr int kJaccardPct    = 50;   // near-duplicate threshold, percent
inline constexpr int kClosureBudget = 2048; // visited-vertex cap per closure scan
inline constexpr int kFanoutCap     = 16;   // exit-candidate samples per out member

struct RegionReport {
    int root_id = 0;              // seed root object id
    std::vector<int> member_ids;  // sorted ascending
    int n = 0, in = 0, out = 0;
    int lb = 0, gap = 0;
    int rank_min = 0, rank_max = 0;
    double q = 0.0;
};

struct Result {
    bool ok = false;                    // false: preconditions failed
    std::vector<RegionReport> regions;  // Q-descending, Jaccard-deduped
};

Result RunHive(Abc_Ntk_t *pNtk, const Config &cfg);   // core, unit-testable
bool ApplyHive(Abc_Frame_t *pAbc, const Config &cfg); // checks + RunHive + report

} // namespace fox::hive

#endif // HIVE_HPP
```

- [ ] **Step 2: Write stub `src/hive/hive.cpp`**

```cpp
#include "hive/hive.hpp"

#include "base/abc/abc.h"

namespace fox::hive {

Result RunHive(Abc_Ntk_t *pNtk, const Config &cfg)
{
    (void)cfg;
    Result res;
    if (!pNtk || !Abc_NtkIsLogic(pNtk))
        return res;
    res.ok = true;
    return res;
}

bool ApplyHive(Abc_Frame_t *pAbc, const Config &cfg)
{
    Abc_Ntk_t *pNtk = Abc_FrameReadNtk(pAbc);
    if (!pNtk)
    {
        printf("hive: network is null\n");
        return false;
    }
    if (!Abc_NtkIsLogic(pNtk))
    {
        printf("hive: network must be logic (run if -K first)\n");
        return false;
    }
    Result res = RunHive(pNtk, cfg);
    return res.ok;
}

} // namespace fox::hive
```

- [ ] **Step 3: Write `src/hive/CMakeLists.txt`** (clone of pdecomp's pattern)

```cmake
add_library(hive hive.cpp hive_graph.cpp region.cpp convex.cpp)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-fexceptions)

target_link_libraries(hive PRIVATE libabc)
target_include_directories(hive PUBLIC ${CMAKE_SOURCE_DIR}/abc/src ${CMAKE_SOURCE_DIR})
```

Note: `hive_graph.cpp`, `region.cpp`, `convex.cpp` do not exist yet. Create three placeholder files now so the target builds; Tasks 2/3/5 fill them:

```cpp
// src/hive/hive_graph.cpp  (placeholder; replaced in Task 2)
```
```cpp
// src/hive/region.cpp      (placeholder; replaced in Task 3)
```
```cpp
// src/hive/convex.cpp      (placeholder; replaced in Task 5)
```

- [ ] **Step 4: Wire into `src/CMakeLists.txt`**

Three edits, following the existing pattern exactly:

1. After `add_subdirectory(fmpart)` add:
```cmake
add_subdirectory(hive)
```
2. In `target_link_libraries(FoxSYN PRIVATE ...)` add `hive` after `fmpart`.
3. At the end, after the `test_fmpart` block, add:
```cmake
add_executable(test_hive "test_hive.cpp")
target_link_libraries(test_hive PRIVATE hive libabc timer)
```

- [ ] **Step 5: Write `src/test_hive.cpp` harness with first test**

```cpp
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

#include "base/abc/abc.h"
#include "base/io/ioAbc.h"
#include "hive/hive.hpp"

namespace {

int g_fail = 0;

void ExpectEq(const char *label, long actual, long expected)
{
    if (actual != expected) {
        std::fprintf(stderr, "FAIL %s: expected %ld, got %ld\n", label, expected, actual);
        ++g_fail;
    }
}

void ExpectTrue(const char *label, bool cond)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL %s\n", label);
        ++g_fail;
    }
}

void ExpectNear(const char *label, double actual, double expected)
{
    if (std::fabs(actual - expected) > 1e-9) {
        std::fprintf(stderr, "FAIL %s: expected %.9f, got %.9f\n", label, expected, actual);
        ++g_fail;
    }
}

// helper: give a node an AND SOP over its current fanins (test_csr3.cpp precedent)
void SetAnd(Abc_Obj_t *n)
{
    auto *pMan = static_cast<Mem_Flex_t *>(n->pNtk->pManFunc);
    if (Abc_ObjFaninNum(n) == 1) n->pData = Abc_SopCreateBuf(pMan);
    else n->pData = Abc_SopCreateAnd(pMan, Abc_ObjFaninNum(n), nullptr);
}

void TestConfigDefaults()
{
    fox::hive::Config cfg;
    ExpectEq("default -N", cfg.num_regions, 20);
    ExpectEq("default -M", cfg.max_nodes, 64);
    ExpectEq("default -I", cfg.max_in, 32);
    ExpectEq("default -O", cfg.max_out, 8);
    ExpectEq("default -K", cfg.lut_k, 6);
    ExpectEq("default -S", cfg.num_seeds, 2000);
    ExpectTrue("default -v off", !cfg.verbose);
}

void TestRunHivePreconditions()
{
    ExpectTrue("null ntk rejected", !fox::hive::RunHive(nullptr, {}).ok);
    Abc_Ntk_t *pStrash = Abc_NtkAlloc(ABC_NTK_STRASH, ABC_FUNC_AIG, 1);
    ExpectTrue("strash ntk rejected", !fox::hive::RunHive(pStrash, {}).ok);
    Abc_NtkDelete(pStrash);
    // PI -> PO only, no internal nodes: ok=true, zero regions (spec 6)
    Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *po = Abc_NtkCreatePo(pNtk);
    Abc_ObjAddFanin(po, a);
    fox::hive::Result r = fox::hive::RunHive(pNtk, {});
    ExpectTrue("no-node ok", r.ok);
    ExpectEq("no-node regions", (long)r.regions.size(), 0);
    Abc_NtkDelete(pNtk);
}

} // namespace

int main()
{
    Abc_Start();
    TestConfigDefaults();
    TestRunHivePreconditions();
    if (g_fail == 0) std::printf("all hive tests passed\n");
    const int result = g_fail == 0 ? 0 : 1;
    Abc_Stop();
    return result;
}
```

- [ ] **Step 6: Build and run**

Run from `/home/longfei/FoxSYN`: `make release 2>&1 | tail -5 && ./release/test_hive`
Expected: build succeeds, `all hive tests passed`.

- [ ] **Step 7: Commit**

```bash
git add src/hive/ src/CMakeLists.txt src/test_hive.cpp
git commit -m "hive: add module scaffolding, public interface, test harness"
```

---

### Task 2: CombGraph

**Files:**
- Create: `src/hive/hive_graph.hpp`; Replace: `src/hive/hive_graph.cpp`
- Test: `src/test_hive.cpp`

**Interfaces:**
- Produces `class fox::hive::CombGraph`:
  - `explicit CombGraph(Abc_Ntk_t *pNtk)`
  - `Abc_Ntk_t *ntk() const`
  - `int max_id() const` — `Abc_NtkObjNumMax` at build time
  - `bool acyclic() const` — false if a combinational loop was found
  - `bool is_vertex(int id) const`
  - `int rank(int id) const`
  - `const std::vector<int> &vertices() const` — all vertex ids ascending
  - `const std::vector<int> &preds(int id) const` — deduped vertex predecessors
  - `const std::vector<int> &succs(int id) const` — deduped vertex successors
  - `const std::vector<int> &ext_ins(int id) const` — deduped non-vertex drivers (PI/BO ids)
  - `bool has_ext_out(int id) const` — node drives a CO (PO or BI) or any non-vertex fanout
- Consumed by Tasks 3, 5, 6, 7, 9.

- [ ] **Step 1: Add failing tests to `src/test_hive.cpp`**

Add `#include "hive/hive_graph.hpp"` and these tests (call them from `main` after the Task 1 tests):

```cpp
// Diamond: a,b -> n1 -> {n2,n3} -> n4 -> PO
struct Diamond {
    Abc_Ntk_t *ntk;
    Abc_Obj_t *a, *b, *n1, *n2, *n3, *n4, *po;
};

Diamond BuildDiamond()
{
    Diamond d;
    d.ntk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    d.a = Abc_NtkCreatePi(d.ntk);
    d.b = Abc_NtkCreatePi(d.ntk);
    d.n1 = Abc_NtkCreateNode(d.ntk);
    Abc_ObjAddFanin(d.n1, d.a); Abc_ObjAddFanin(d.n1, d.b); SetAnd(d.n1);
    d.n2 = Abc_NtkCreateNode(d.ntk);
    Abc_ObjAddFanin(d.n2, d.n1); SetAnd(d.n2);
    d.n3 = Abc_NtkCreateNode(d.ntk);
    Abc_ObjAddFanin(d.n3, d.n1); SetAnd(d.n3);
    d.n4 = Abc_NtkCreateNode(d.ntk);
    Abc_ObjAddFanin(d.n4, d.n2); Abc_ObjAddFanin(d.n4, d.n3); SetAnd(d.n4);
    d.po = Abc_NtkCreatePo(d.ntk);
    Abc_ObjAddFanin(d.po, d.n4);
    return d;
}

// Global invariant every CombGraph test runs: rank strictly increases on
// every edge (this is what replaces ABC's Level, spec 3.3/5.1).
void CheckRankStrict(const fox::hive::CombGraph &g, const char *label)
{
    for (int v : g.vertices())
        for (int s : g.succs(v))
            if (!(g.rank(s) > g.rank(v))) {
                std::fprintf(stderr, "FAIL %s: rank(%d)=%d !> rank(%d)=%d\n",
                             label, s, g.rank(s), v, g.rank(v));
                ++g_fail;
            }
}

void TestCombGraphDiamond()
{
    Diamond d = BuildDiamond();
    fox::hive::CombGraph g(d.ntk);
    ExpectTrue("acyclic", g.acyclic());
    ExpectEq("4 vertices", (long)g.vertices().size(), 4);
    ExpectTrue("n1 vertex", g.is_vertex(Abc_ObjId(d.n1)));
    ExpectTrue("pi not vertex", !g.is_vertex(Abc_ObjId(d.a)));
    ExpectTrue("po not vertex", !g.is_vertex(Abc_ObjId(d.po)));
    ExpectEq("n1 rank", g.rank(Abc_ObjId(d.n1)), 0);
    ExpectEq("n2 rank", g.rank(Abc_ObjId(d.n2)), 1);
    ExpectEq("n4 rank", g.rank(Abc_ObjId(d.n4)), 2);
    ExpectEq("n1 ext_ins", (long)g.ext_ins(Abc_ObjId(d.n1)).size(), 2);
    ExpectEq("n1 preds", (long)g.preds(Abc_ObjId(d.n1)).size(), 0);
    ExpectEq("n1 succs", (long)g.succs(Abc_ObjId(d.n1)).size(), 2);
    ExpectTrue("n4 ext_out", g.has_ext_out(Abc_ObjId(d.n4)));
    ExpectTrue("n1 no ext_out", !g.has_ext_out(Abc_ObjId(d.n1)));
    CheckRankStrict(g, "diamond rank");
    Abc_NtkDelete(d.ntk);
}

void TestCombGraphDedup()
{
    // nd reads PI a twice; nx reads n1 twice -> one adjacency edge each (spec 4.2)
    Abc_Ntk_t *p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(p);
    Abc_Obj_t *n1 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(n1, a); SetAnd(n1);
    Abc_Obj_t *nd = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(nd, a); Abc_ObjAddFanin(nd, a); SetAnd(nd);
    Abc_Obj_t *nx = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(nx, n1); Abc_ObjAddFanin(nx, n1); SetAnd(nx);
    Abc_Obj_t *po = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po, nx);
    Abc_Obj_t *po2 = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po2, nd);
    fox::hive::CombGraph g(p);
    ExpectEq("dup PI dedup", (long)g.ext_ins(Abc_ObjId(nd)).size(), 1);
    ExpectEq("dup vertex dedup", (long)g.preds(Abc_ObjId(nx)).size(), 1);
    ExpectEq("succs dedup", (long)g.succs(Abc_ObjId(n1)).size(), 1);
    CheckRankStrict(g, "dedup rank");
    Abc_NtkDelete(p);
}

void TestCombGraphLatch()
{
    // n1 -> [BI latch BO] -> n2; latch truncates both directions (spec 5.1)
    Abc_Ntk_t *p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(p);
    Abc_Obj_t *n1 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(n1, a); SetAnd(n1);
    Abc_Obj_t *bo = Abc_NtkAddLatch(p, n1, ABC_INIT_ZERO);  // creates BO<-latch<-BI<-n1
    Abc_Obj_t *n2 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(n2, bo); SetAnd(n2);
    Abc_Obj_t *po = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po, n2);
    fox::hive::CombGraph g(p);
    ExpectTrue("bo not vertex", !g.is_vertex(Abc_ObjId(bo)));
    ExpectEq("n2 preds empty", (long)g.preds(Abc_ObjId(n2)).size(), 0);
    ExpectEq("n2 ext_ins = {BO}", (long)g.ext_ins(Abc_ObjId(n2)).size(), 1);
    ExpectEq("n2 ext_in id", g.ext_ins(Abc_ObjId(n2))[0], (long)Abc_ObjId(bo));
    ExpectTrue("n1 drives BI -> ext_out", g.has_ext_out(Abc_ObjId(n1)));
    ExpectEq("n1 succs empty", (long)g.succs(Abc_ObjId(n1)).size(), 0);
    CheckRankStrict(g, "latch rank");
    Abc_NtkDelete(p);
}

void TestCombGraphBufferRankAndConst()
{
    // Buffer chain: rank must still increment on 1-fanin nodes (ABC's Level
    // would not for barbuf-shaped nodes, spec 3.3); const1 is an ordinary node.
    Abc_Ntk_t *p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(p);
    Abc_Obj_t *b1 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(b1, a); SetAnd(b1);          // 1-fanin buffer
    Abc_Obj_t *b2 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(b2, b1); SetAnd(b2);         // 1-fanin buffer
    Abc_Obj_t *c = Abc_NtkCreateNodeConst1(p);
    Abc_Obj_t *n = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(n, b2); Abc_ObjAddFanin(n, c); SetAnd(n);
    Abc_Obj_t *po = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po, n);
    fox::hive::CombGraph g(p);
    ExpectEq("buf rank +1", g.rank(Abc_ObjId(b2)), g.rank(Abc_ObjId(b1)) + 1);
    ExpectTrue("const1 is vertex", g.is_vertex(Abc_ObjId(c)));
    ExpectTrue("const1 is a preds member of n",
               std::find(g.preds(Abc_ObjId(n)).begin(), g.preds(Abc_ObjId(n)).end(),
                         (int)Abc_ObjId(c)) != g.preds(Abc_ObjId(n)).end());
    CheckRankStrict(g, "buffer rank");
    Abc_NtkDelete(p);
}
```

- [ ] **Step 2: Build to verify failure**

Run: `make release 2>&1 | grep -E "error|hive_graph" | head -5`
Expected: FAIL — `hive/hive_graph.hpp: No such file or directory`.

- [ ] **Step 3: Write `src/hive/hive_graph.hpp`**

```cpp
#ifndef HIVE_GRAPH_HPP
#define HIVE_GRAPH_HPP

#include <vector>

#include "base/abc/abc.h"

namespace fox::hive {

// Combinational-graph snapshot of a logic network (docs/hive-design.md 5.1).
// Vertices: every internal node (Abc_ObjIsNode), constants included.
// PI/PO/BI/BO/latch are boundary objects, never vertices; traversal stops at
// them, which is what makes a latch break combinational paths. Adjacency is
// deduplicated by object id. rank() strictly increases along every edge and
// is independent of ABC's Level field. Snapshot: built once, never updated.
class CombGraph {
public:
    explicit CombGraph(Abc_Ntk_t *pNtk);

    Abc_Ntk_t *ntk() const { return m_ntk; }
    int max_id() const { return m_max_id; }
    bool acyclic() const { return m_acyclic; }
    bool is_vertex(int id) const { return m_is_vertex[id] != 0; }
    int rank(int id) const { return m_rank[id]; }
    const std::vector<int> &vertices() const { return m_vertices; }
    const std::vector<int> &preds(int id) const { return m_preds[id]; }
    const std::vector<int> &succs(int id) const { return m_succs[id]; }
    const std::vector<int> &ext_ins(int id) const { return m_ext_ins[id]; }
    bool has_ext_out(int id) const { return m_ext_out[id] != 0; }

private:
    Abc_Ntk_t *m_ntk = nullptr;
    int m_max_id = 0;
    bool m_acyclic = true;
    std::vector<char> m_is_vertex, m_ext_out;
    std::vector<int> m_rank;
    std::vector<int> m_vertices;
    std::vector<std::vector<int>> m_preds, m_succs, m_ext_ins;
};

} // namespace fox::hive

#endif // HIVE_GRAPH_HPP
```

- [ ] **Step 4: Write `src/hive/hive_graph.cpp`**

```cpp
#include "hive/hive_graph.hpp"

#include <algorithm>
#include <queue>

namespace fox::hive {

CombGraph::CombGraph(Abc_Ntk_t *pNtk)
    : m_ntk(pNtk)
{
    m_max_id = Abc_NtkObjNumMax(pNtk);
    m_is_vertex.assign(m_max_id, 0);
    m_ext_out.assign(m_max_id, 0);
    m_rank.assign(m_max_id, 0);
    m_preds.assign(m_max_id, {});
    m_succs.assign(m_max_id, {});
    m_ext_ins.assign(m_max_id, {});

    Abc_Obj_t *pObj;
    int i;
    Abc_NtkForEachNode(pNtk, pObj, i)
    {
        m_is_vertex[Abc_ObjId(pObj)] = 1;
        m_vertices.push_back((int)Abc_ObjId(pObj));
    }

    Abc_NtkForEachNode(pNtk, pObj, i)
    {
        const int id = (int)Abc_ObjId(pObj);
        Abc_Obj_t *pFanin;
        int k;
        Abc_ObjForEachFanin(pObj, pFanin, k)
        {
            const int fid = (int)Abc_ObjId(pFanin);
            if (m_is_vertex[fid])
                m_preds[id].push_back(fid);
            else
                m_ext_ins[id].push_back(fid);   // PI or BO
        }
        auto dedup = [](std::vector<int> &v)
        {
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
        };
        dedup(m_preds[id]);
        dedup(m_ext_ins[id]);

        Abc_Obj_t *pFanout;
        Abc_ObjForEachFanout(pObj, pFanout, k)
            if (!m_is_vertex[Abc_ObjId(pFanout)])
                m_ext_out[id] = 1;              // PO or BI (or any non-vertex)
    }

    // transpose for succs (keeps preds/succs mutually consistent)
    for (int v : m_vertices)
        for (int p : m_preds[v])
            m_succs[p].push_back(v);
    for (int v : m_vertices)
        std::sort(m_succs[v].begin(), m_succs[v].end());

    // rank: Kahn over vertex preds; rank(v) = 1 + max(rank(pred)), 0 if none
    std::vector<int> indeg(m_max_id, 0);
    for (int v : m_vertices)
        indeg[v] = (int)m_preds[v].size();
    std::queue<int> q;
    for (int v : m_vertices)
        if (indeg[v] == 0)
            q.push(v);
    int processed = 0;
    while (!q.empty())
    {
        const int v = q.front();
        q.pop();
        ++processed;
        for (int s : m_succs[v])
        {
            m_rank[s] = std::max(m_rank[s], m_rank[v] + 1);
            if (--indeg[s] == 0)
                q.push(s);
        }
    }
    m_acyclic = processed == (int)m_vertices.size();
}

} // namespace fox::hive
```

- [ ] **Step 5: Build and run**

Run: `make release 2>&1 | tail -3 && ./release/test_hive`
Expected: `all hive tests passed`.

- [ ] **Step 6: Commit**

```bash
git add src/hive/hive_graph.hpp src/hive/hive_graph.cpp src/test_hive.cpp
git commit -m "hive: add CombGraph snapshot with dedup adjacency and strict rank"
```

---

### Task 3: Region — membership, incremental in/out, Q

**Files:**
- Create: `src/hive/region.hpp`; Replace: `src/hive/region.cpp`
- Test: `src/test_hive.cpp`

**Interfaces:**
- Produces `struct fox::hive::Metrics { int n, in, out; double q; int lb, gap; int rank_min, rank_max; }` (lb/gap stay 0 until Task 4) and `class fox::hive::Region`:
  - `explicit Region(const CombGraph &g)`
  - `void init(const std::vector<int> &memberIds)` — reset then `add` each
  - `void add(int id)` — incremental absorb of one vertex
  - `bool contains(int id) const`, `int size() const`
  - `std::vector<int> member_ids() const` — sorted ascending
  - `std::vector<int> in_objects() const` — sorted distinct external drivers (any type)
  - `std::vector<int> entrance_candidates() const` — `in_objects()` filtered to vertices
  - `std::vector<int> out_members() const` — sorted members with an external consumer
  - `Metrics metrics(int lut_k) const` — from maintained sets
  - `Metrics recompute(int lut_k) const` — independent from-scratch scan (test cross-check)
- Region is cheap to copy (small hash containers only); trial moves copy it.
- Consumed by Tasks 4, 5, 6, 7, 9.

- [ ] **Step 1: Add failing tests**

Add `#include "hive/region.hpp"` and:

```cpp
void TestRegionBoundary()
{
    Diamond d = BuildDiamond();
    fox::hive::CombGraph g(d.ntk);
    fox::hive::Region r(g);
    // {n2,n3,n4}: in = {n1} (n1 drives two members but counts once, spec 2.2)
    r.init({(int)Abc_ObjId(d.n2), (int)Abc_ObjId(d.n3), (int)Abc_ObjId(d.n4)});
    fox::hive::Metrics m = r.metrics(6);
    ExpectEq("in dedup by driver", m.in, 1);
    ExpectEq("out = n4 (drives PO)", m.out, 1);
    ExpectEq("n", m.n, 3);
    ExpectNear("q = 3/2", m.q, 1.5);
    // mixed fanout: {n1,n2}: n1 out (n3 external); absorb n3 -> n1 leaves out
    fox::hive::Region r2(g);
    r2.init({(int)Abc_ObjId(d.n1), (int)Abc_ObjId(d.n2)});
    ExpectEq("n1+n2 out", r2.metrics(6).out, 2);   // n1 (n3 ext), n2 (n4 ext)
    r2.add((int)Abc_ObjId(d.n3));
    ExpectEq("after n3: out", r2.metrics(6).out, 2); // n1 internal now; n2,n3 -> n4 ext
    r2.add((int)Abc_ObjId(d.n4));
    ExpectEq("after n4: out", r2.metrics(6).out, 1); // only n4 (PO)
    ExpectEq("after n4: in", r2.metrics(6).in, 2);   // a, b
    Abc_NtkDelete(d.ntk);
}

void TestRegionConst1Ordinary()
{
    // const1 external: counts in `in` and is an entrance candidate (spec 2.1)
    Abc_Ntk_t *p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(p);
    Abc_Obj_t *c = Abc_NtkCreateNodeConst1(p);
    Abc_Obj_t *n = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(n, a); Abc_ObjAddFanin(n, c); SetAnd(n);
    Abc_Obj_t *po = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po, n);
    fox::hive::CombGraph g(p);
    fox::hive::Region r(g);
    r.init({(int)Abc_ObjId(n)});
    ExpectEq("const1 in `in`", r.metrics(6).in, 2);
    std::vector<int> cands = r.entrance_candidates();
    ExpectEq("const1 is the only entrance candidate", (long)cands.size(), 1);
    ExpectEq("candidate is const1", cands[0], (long)Abc_ObjId(c));
    r.add((int)Abc_ObjId(c));
    ExpectEq("absorbed: in", r.metrics(6).in, 1);
    ExpectEq("absorbed: n", r.metrics(6).n, 2);
    Abc_NtkDelete(p);
}

void TestRegionIncrementalVsRecompute()
{
    Diamond d = BuildDiamond();
    fox::hive::CombGraph g(d.ntk);
    fox::hive::Region r(g);
    r.init({(int)Abc_ObjId(d.n1)});
    // deterministic walk: repeatedly absorb the smallest-id vertex neighbor
    for (int step = 0; step < 3; ++step)
    {
        std::vector<int> cands = r.entrance_candidates();
        for (int om : r.out_members())
            for (int s : g.succs(om))
                if (!r.contains(s))
                    cands.push_back(s);
        std::sort(cands.begin(), cands.end());
        cands.erase(std::unique(cands.begin(), cands.end()), cands.end());
        if (cands.empty()) break;
        r.add(cands[0]);
        fox::hive::Metrics a = r.metrics(6), b = r.recompute(6);
        ExpectEq("inc n == recompute n", a.n, b.n);
        ExpectEq("inc in == recompute in", a.in, b.in);
        ExpectEq("inc out == recompute out", a.out, b.out);
        ExpectNear("inc q == recompute q", a.q, b.q);
    }
    Abc_NtkDelete(d.ntk);
}
```

- [ ] **Step 2: Build to verify failure**

Run: `make release 2>&1 | grep error | head -3`
Expected: FAIL — `hive/region.hpp: No such file or directory`.

- [ ] **Step 3: Write `src/hive/region.hpp`**

```cpp
#ifndef HIVE_REGION_HPP
#define HIVE_REGION_HPP

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "hive/hive_graph.hpp"

namespace fox::hive {

struct Metrics {
    int n = 0, in = 0, out = 0;
    double q = 0.0;   // n / (in + out); 0 when the boundary is empty
    int lb = 0, gap = 0;
    int rank_min = 0, rank_max = 0;
};

// Incrementally maintained region (docs/hive-design.md 2.2, 5). Uses only
// small hash containers keyed by object id, so copying a Region for a trial
// move is cheap; the CombGraph holds all id-indexed bulk data.
class Region {
public:
    explicit Region(const CombGraph &g) : m_g(&g) {}

    void init(const std::vector<int> &memberIds);
    void add(int id);
    bool contains(int id) const { return m_members.count(id) != 0; }
    int size() const { return (int)m_members.size(); }

    std::vector<int> member_ids() const;
    std::vector<int> in_objects() const;
    std::vector<int> entrance_candidates() const;
    std::vector<int> out_members() const;

    Metrics metrics(int lut_k) const;
    Metrics recompute(int lut_k) const;  // independent path, for cross-checks

private:
    int lower_bound_luts(int lut_k) const;  // Task 4; returns 0 until then

    const CombGraph *m_g;
    std::unordered_set<int> m_members;
    // external driver id -> number of members it drives (never contains members)
    std::unordered_map<int, int> m_driven_cnt;
    // member id -> number of vertex succs outside the region
    std::unordered_map<int, int> m_ext_succ;
    int m_in = 0;   // |m_driven_cnt| keys with cnt > 0 (all keys qualify)
    int m_out = 0;  // members with has_ext_out || ext_succ > 0
};

} // namespace fox::hive

#endif // HIVE_REGION_HPP
```

- [ ] **Step 4: Write `src/hive/region.cpp`**

```cpp
#include "hive/region.hpp"

#include <algorithm>

namespace fox::hive {

void Region::init(const std::vector<int> &memberIds)
{
    m_members.clear();
    m_driven_cnt.clear();
    m_ext_succ.clear();
    m_in = 0;
    m_out = 0;
    for (int id : memberIds)
        add(id);
}

void Region::add(int id)
{
    const CombGraph &g = *m_g;
    m_members.insert(id);

    // id stops being an external driver
    auto it = m_driven_cnt.find(id);
    if (it != m_driven_cnt.end())
    {
        m_driven_cnt.erase(it);
        --m_in;
    }

    // id's drivers gain one driven member
    auto bump = [&](int d)
    {
        int &c = m_driven_cnt[d];
        if (++c == 1)
            ++m_in;
    };
    for (int p : g.preds(id))
        if (!contains(p))
            bump(p);
    for (int x : g.ext_ins(id))
        bump(x);   // PI/BO: never members

    // member predecessors lose one external successor
    for (int p : g.preds(id))
    {
        if (!contains(p) || p == id)
            continue;
        int &c = m_ext_succ[p];
        --c;
        if (c == 0 && !g.has_ext_out(p))
            --m_out;   // p leaves the out set
    }

    // id's own out status
    int cnt = 0;
    for (int s : g.succs(id))
        if (!contains(s))
            ++cnt;
    m_ext_succ[id] = cnt;
    if (g.has_ext_out(id) || cnt > 0)
        ++m_out;
}

std::vector<int> Region::member_ids() const
{
    std::vector<int> v(m_members.begin(), m_members.end());
    std::sort(v.begin(), v.end());
    return v;
}

std::vector<int> Region::in_objects() const
{
    std::vector<int> v;
    v.reserve(m_driven_cnt.size());
    for (const auto &kv : m_driven_cnt)
        v.push_back(kv.first);
    std::sort(v.begin(), v.end());
    return v;
}

std::vector<int> Region::entrance_candidates() const
{
    std::vector<int> v;
    for (const auto &kv : m_driven_cnt)
        if (m_g->is_vertex(kv.first))
            v.push_back(kv.first);
    std::sort(v.begin(), v.end());
    return v;
}

std::vector<int> Region::out_members() const
{
    std::vector<int> v;
    for (const auto &kv : m_ext_succ)
        if (m_g->has_ext_out(kv.first) || kv.second > 0)
            v.push_back(kv.first);
    std::sort(v.begin(), v.end());
    return v;
}

Metrics Region::metrics(int lut_k) const
{
    Metrics m;
    m.n = (int)m_members.size();
    m.in = m_in;
    m.out = m_out;
    const int b = m.in + m.out;
    m.q = b > 0 ? (double)m.n / b : 0.0;
    bool first = true;
    for (int id : m_members)
    {
        const int rk = m_g->rank(id);
        if (first) { m.rank_min = m.rank_max = rk; first = false; }
        else
        {
            m.rank_min = std::min(m.rank_min, rk);
            m.rank_max = std::max(m.rank_max, rk);
        }
    }
    m.lb = lower_bound_luts(lut_k);
    m.gap = m.n - m.lb;
    return m;
}

Metrics Region::recompute(int lut_k) const
{
    // From-scratch scan; deliberately shares no state with the incremental
    // bookkeeping so tests can cross-check it (docs/hive-design.md 8).
    Metrics m;
    m.n = (int)m_members.size();
    std::unordered_set<int> drivers;
    for (int id : m_members)
    {
        for (int p : m_g->preds(id))
            if (!m_members.count(p))
                drivers.insert(p);
        for (int x : m_g->ext_ins(id))
            drivers.insert(x);
        bool is_out = m_g->has_ext_out(id);
        if (!is_out)
            for (int s : m_g->succs(id))
                if (!m_members.count(s)) { is_out = true; break; }
        if (is_out)
            ++m.out;
    }
    m.in = (int)drivers.size();
    const int b = m.in + m.out;
    m.q = b > 0 ? (double)m.n / b : 0.0;
    bool first = true;
    for (int id : m_members)
    {
        const int rk = m_g->rank(id);
        if (first) { m.rank_min = m.rank_max = rk; first = false; }
        else
        {
            m.rank_min = std::min(m.rank_min, rk);
            m.rank_max = std::max(m.rank_max, rk);
        }
    }
    m.lb = lower_bound_luts(lut_k);
    m.gap = m.n - m.lb;
    return m;
}

int Region::lower_bound_luts(int lut_k) const
{
    (void)lut_k;
    return 0;   // Task 4
}

} // namespace fox::hive
```

- [ ] **Step 5: Build and run**

Run: `make release 2>&1 | tail -3 && ./release/test_hive`
Expected: `all hive tests passed`.

- [ ] **Step 6: Commit**

```bash
git add src/hive/region.hpp src/hive/region.cpp src/test_hive.cpp
git commit -m "hive: add Region with incremental in/out maintenance and Q"
```

---

### Task 4: Region — LB and gap

**Files:**
- Modify: `src/hive/region.cpp` (`lower_bound_luts`)
- Test: `src/test_hive.cpp`

**Interfaces:**
- Consumes: Task 3's `Region`. Produces: `Metrics.lb`/`Metrics.gap` computed per spec §2.4:
  `LB = max(out, ceil_or_0((in − out)/(K−1)), max_j ceil_or_0((supp_j − 1)/(K−1)))`.

- [ ] **Step 1: Add failing tests**

```cpp
void TestLbThreeTerms()
{
    // Term 1 wins: 3 parallel PI->node->PO columns; in=3 out=3, K=6 -> LB=3
    Abc_Ntk_t *p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    std::vector<int> ids;
    for (int i = 0; i < 3; ++i)
    {
        Abc_Obj_t *pi = Abc_NtkCreatePi(p);
        Abc_Obj_t *n = Abc_NtkCreateNode(p);
        Abc_ObjAddFanin(n, pi); SetAnd(n);
        Abc_Obj_t *po = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po, n);
        ids.push_back((int)Abc_ObjId(n));
    }
    {
        fox::hive::CombGraph g(p);
        fox::hive::Region r(g);
        r.init(ids);
        ExpectEq("term1 lb", r.metrics(6).lb, 3);
        ExpectEq("term1 gap", r.metrics(6).gap, 0);
    }
    Abc_NtkDelete(p);

    // Term 2 wins: j1(6 fresh PIs), j2(6 other fresh PIs), both -> PO. K=4:
    // in=12 out=2: t1=2, t2=ceil(10/3)=4, t3=ceil(5/3)=2 -> LB=4
    p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    ids.clear();
    for (int j = 0; j < 2; ++j)
    {
        Abc_Obj_t *n = Abc_NtkCreateNode(p);
        for (int i = 0; i < 6; ++i)
            Abc_ObjAddFanin(n, Abc_NtkCreatePi(p));
        SetAnd(n);
        Abc_Obj_t *po = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po, n);
        ids.push_back((int)Abc_ObjId(n));
    }
    {
        fox::hive::CombGraph g(p);
        fox::hive::Region r(g);
        r.init(ids);
        ExpectEq("term2 lb", r.metrics(4).lb, 4);
    }
    Abc_NtkDelete(p);

    // Term 3 wins: o1,o2 both read the same 6 PIs, both -> PO. K=3:
    // in=6 out=2: t1=2, t2=ceil(4/2)=2, t3=ceil(5/2)=3 -> LB=3
    p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    ids.clear();
    std::vector<Abc_Obj_t *> pis;
    for (int i = 0; i < 6; ++i)
        pis.push_back(Abc_NtkCreatePi(p));
    for (int j = 0; j < 2; ++j)
    {
        Abc_Obj_t *n = Abc_NtkCreateNode(p);
        for (Abc_Obj_t *pi : pis)
            Abc_ObjAddFanin(n, pi);
        SetAnd(n);
        Abc_Obj_t *po = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po, n);
        ids.push_back((int)Abc_ObjId(n));
    }
    {
        fox::hive::CombGraph g(p);
        fox::hive::Region r(g);
        r.init(ids);
        ExpectEq("term3 lb", r.metrics(3).lb, 3);
    }
    Abc_NtkDelete(p);
}

void TestLbEdgeCases()
{
    // (in - out) <= 0 -> term2 is 0, no negative ceiling: p1,p2 both read one
    // PI, both -> PO: in=1 out=2 -> LB = max(2, 0, 0) = 2
    Abc_Ntk_t *p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(p);
    std::vector<int> ids;
    for (int j = 0; j < 2; ++j)
    {
        Abc_Obj_t *n = Abc_NtkCreateNode(p);
        Abc_ObjAddFanin(n, a); SetAnd(n);
        Abc_Obj_t *po = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po, n);
        ids.push_back((int)Abc_ObjId(n));
    }
    {
        fox::hive::CombGraph g(p);
        fox::hive::Region r(g);
        r.init(ids);
        ExpectEq("in<out lb", r.metrics(6).lb, 2);
    }
    Abc_NtkDelete(p);

    // out = 0 (dead region): u1(PI)->u2, u2 no fanout. in=1 out=0:
    // LB = max(0, ceil(1/5)=1, 0 since no outputs) = 1
    p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *b = Abc_NtkCreatePi(p);
    Abc_Obj_t *u1 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(u1, b); SetAnd(u1);
    Abc_Obj_t *u2 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(u2, u1); SetAnd(u2);
    {
        fox::hive::CombGraph g(p);
        fox::hive::Region r(g);
        r.init({(int)Abc_ObjId(u1), (int)Abc_ObjId(u2)});
        ExpectEq("dead out", r.metrics(6).out, 0);
        ExpectEq("dead lb", r.metrics(6).lb, 1);
    }
    Abc_NtkDelete(p);
}

void TestLbFunctionalCounterexample()
{
    // Executable documentation of spec 2.5 limitation 1: LB is NOT a lower
    // bound for functional resynthesis. x = LUT(a,b) ignoring b (SOP "1- 1"),
    // y = LUT(x,c) ignoring c. Structurally in=3 out=1 supp=3, K=2 ->
    // LB = max(1, ceil(2/1), ceil(2/1)) = 2, yet the region computes just `a`
    // (one LUT, even a wire). Do NOT reintroduce a gap<=0 exclusion rule.
    Abc_Ntk_t *p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    auto *pMan = static_cast<Mem_Flex_t *>(p->pManFunc);
    Abc_Obj_t *a = Abc_NtkCreatePi(p);
    Abc_Obj_t *b = Abc_NtkCreatePi(p);
    Abc_Obj_t *c = Abc_NtkCreatePi(p);
    Abc_Obj_t *x = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(x, a); Abc_ObjAddFanin(x, b);
    x->pData = Abc_SopRegister(pMan, "1- 1\n");
    Abc_Obj_t *y = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(y, x); Abc_ObjAddFanin(y, c);
    y->pData = Abc_SopRegister(pMan, "1- 1\n");
    Abc_Obj_t *po = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po, y);
    fox::hive::CombGraph g(p);
    fox::hive::Region r(g);
    r.init({(int)Abc_ObjId(x), (int)Abc_ObjId(y)});
    ExpectEq("counterexample lb", r.metrics(2).lb, 2);  // true functional need: 1
    Abc_NtkDelete(p);
}

void TestLbHandVerifiedOptimal()
{
    // LB <= hand-derived optimal m under model M1-M3 (spec 2.4).
    // (i) 7-input AND as 2-node tree, K=6: in=7 out=1 ->
    //     LB = max(1, ceil(6/5)=2, ceil(6/5)=2) = 2; optimal m = 2.
    Abc_Ntk_t *p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *t1 = Abc_NtkCreateNode(p);
    for (int i = 0; i < 6; ++i)
        Abc_ObjAddFanin(t1, Abc_NtkCreatePi(p));
    SetAnd(t1);
    Abc_Obj_t *t2 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(t2, t1); Abc_ObjAddFanin(t2, Abc_NtkCreatePi(p)); SetAnd(t2);
    Abc_Obj_t *po = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po, t2);
    {
        fox::hive::CombGraph g(p);
        fox::hive::Region r(g);
        r.init({(int)Abc_ObjId(t1), (int)Abc_ObjId(t2)});
        const int lb = r.metrics(6).lb;
        ExpectEq("and7 lb", lb, 2);
        ExpectTrue("and7 lb <= optimal 2", lb <= 2);
    }
    Abc_NtkDelete(p);

    // (ii) Diamond {n1..n4}, K=6: in=2 out=1 -> LB=1; optimal m = 1
    //      (a single LUT of (a,b) computes n4).
    Diamond d = BuildDiamond();
    {
        fox::hive::CombGraph g(d.ntk);
        fox::hive::Region r(g);
        r.init({(int)Abc_ObjId(d.n1), (int)Abc_ObjId(d.n2),
                (int)Abc_ObjId(d.n3), (int)Abc_ObjId(d.n4)});
        const int lb = r.metrics(6).lb;
        ExpectEq("diamond lb", lb, 1);
        ExpectTrue("diamond lb <= optimal 1", lb <= 1);
    }
    Abc_NtkDelete(d.ntk);
}
```

Note on spec §8's "small-DAG exhaustive enumeration": implementing an exact LUT-covering optimizer inside the test is a mini-project of its own; this plan replaces it with the three hand-verified-optimal cases above (values derived in comments). This is a deliberate, visible reduction — record it in the commit message.

- [ ] **Step 2: Run to verify failure**

Run: `make release 2>&1 | tail -3 && ./release/test_hive 2>&1 | head -5`
Expected: FAIL lines mentioning `lb` (current stub returns 0).

- [ ] **Step 3: Implement `lower_bound_luts` in `src/hive/region.cpp`**

Replace the stub:

```cpp
int Region::lower_bound_luts(int lut_k) const
{
    const CombGraph &g = *m_g;
    const int km1 = lut_k - 1;
    auto ceil_div_or_0 = [km1](int x)
    {
        return x > 0 ? (x + km1 - 1) / km1 : 0;
    };

    int term1 = m_out;
    int term2 = ceil_div_or_0(m_in - m_out);

    // term3: per-output structural support inside the region (spec 2.4)
    int term3 = 0;
    for (const auto &kv : m_ext_succ)
    {
        const int j = kv.first;
        if (!(g.has_ext_out(j) || kv.second > 0))
            continue;   // not an output
        std::unordered_set<int> supp, seen;
        std::vector<int> stack{j};
        seen.insert(j);
        while (!stack.empty())
        {
            const int v = stack.back();
            stack.pop_back();
            for (int p : g.preds(v))
            {
                if (m_members.count(p))
                {
                    if (seen.insert(p).second)
                        stack.push_back(p);
                }
                else
                    supp.insert(p);
            }
            for (int x : g.ext_ins(v))
                supp.insert(x);
        }
        term3 = std::max(term3, ceil_div_or_0((int)supp.size() - 1));
    }

    return std::max(term1, std::max(term2, term3));
}
```

Add `#include <unordered_set>` to `region.cpp` if not already present via the header.

- [ ] **Step 4: Build and run**

Run: `make release 2>&1 | tail -3 && ./release/test_hive`
Expected: `all hive tests passed`.

- [ ] **Step 5: Commit**

```bash
git add src/hive/region.cpp src/test_hive.cpp
git commit -m "hive: add LUT lower bound and gap under model M1-M3

The exhaustive small-DAG check from the spec's test list is reduced to
three hand-verified-optimal cases; an exact LUT-covering optimizer in
the test harness is not worth its cost for a measurement instrument."
```

---

### Task 5: Convex closure and brute-force checker

**Files:**
- Create: `src/hive/convex.hpp`; Replace: `src/hive/convex.cpp`
- Test: `src/test_hive.cpp`

**Interfaces:**
- Produces:
  - `struct fox::hive::ClosureResult { bool ok; std::vector<int> violators; }` — `ok=false` means budget exceeded, the move must be rejected; `violators` sorted ascending, valid only when `ok`.
  - `ClosureResult ComputeClosure(const CombGraph &g, const Region &r, const std::vector<int> &added, int budget)` — violators of `R ∪ added`, rank-band pruned, `budget` = max marked vertices per direction.
  - `bool IsConvexBrute(const CombGraph &g, const std::vector<int> &memberIds)` — full unbounded reachability, no rank pruning; the independent verification path (spec §8).
- Consumed by Tasks 6, 9.

- [ ] **Step 1: Add failing tests**

Add `#include "hive/convex.hpp"` and:

```cpp
// r1 -> v -> r2 plus r1 -> r2: {r1,r2} is nonconvex, v is the violator.
struct Reconv {
    Abc_Ntk_t *ntk;
    Abc_Obj_t *r1, *v, *r2;
};

Reconv BuildReconv()
{
    Reconv c;
    c.ntk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(c.ntk);
    c.r1 = Abc_NtkCreateNode(c.ntk);
    Abc_ObjAddFanin(c.r1, a); SetAnd(c.r1);
    c.v = Abc_NtkCreateNode(c.ntk);
    Abc_ObjAddFanin(c.v, c.r1); SetAnd(c.v);
    c.r2 = Abc_NtkCreateNode(c.ntk);
    Abc_ObjAddFanin(c.r2, c.r1); Abc_ObjAddFanin(c.r2, c.v); SetAnd(c.r2);
    Abc_Obj_t *po = Abc_NtkCreatePo(c.ntk);
    Abc_ObjAddFanin(po, c.r2);
    return c;
}

void TestClosureSimple()
{
    Reconv c = BuildReconv();
    fox::hive::CombGraph g(c.ntk);
    fox::hive::Region r(g);
    r.init({(int)Abc_ObjId(c.r1), (int)Abc_ObjId(c.r2)});
    ExpectTrue("nonconvex detected by brute",
               !fox::hive::IsConvexBrute(g, r.member_ids()));
    fox::hive::ClosureResult cl =
        fox::hive::ComputeClosure(g, r, {}, fox::hive::kClosureBudget);
    ExpectTrue("closure ok", cl.ok);
    ExpectEq("one violator", (long)cl.violators.size(), 1);
    ExpectEq("violator is v", cl.violators[0], (long)Abc_ObjId(c.v));
    r.add((int)Abc_ObjId(c.v));
    ExpectTrue("convex after absorb", fox::hive::IsConvexBrute(g, r.member_ids()));
    fox::hive::ClosureResult cl2 =
        fox::hive::ComputeClosure(g, r, {}, fox::hive::kClosureBudget);
    ExpectTrue("second closure ok", cl2.ok);
    ExpectEq("second closure no-op", (long)cl2.violators.size(), 0);  // spec 3.2
    Abc_NtkDelete(c.ntk);
}

void TestClosureChainedAndMultiPair()
{
    // r1 -> w1 -> w2 -> r2, r1 -> r2, and r2 -> y -> r3, r2 -> r3:
    // members {r1,r2,r3}, violators {w1,w2,y} in ONE pass (spec 3.2).
    Abc_Ntk_t *p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(p);
    Abc_Obj_t *r1 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(r1, a); SetAnd(r1);
    Abc_Obj_t *w1 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(w1, r1); SetAnd(w1);
    Abc_Obj_t *w2 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(w2, w1); SetAnd(w2);
    Abc_Obj_t *r2 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(r2, r1); Abc_ObjAddFanin(r2, w2); SetAnd(r2);
    Abc_Obj_t *y = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(y, r2); SetAnd(y);
    Abc_Obj_t *r3 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(r3, r2); Abc_ObjAddFanin(r3, y); SetAnd(r3);
    Abc_Obj_t *po = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po, r3);
    fox::hive::CombGraph g(p);
    fox::hive::Region r(g);
    r.init({(int)Abc_ObjId(r1), (int)Abc_ObjId(r2), (int)Abc_ObjId(r3)});
    fox::hive::ClosureResult cl =
        fox::hive::ComputeClosure(g, r, {}, fox::hive::kClosureBudget);
    ExpectTrue("chained ok", cl.ok);
    ExpectEq("three violators", (long)cl.violators.size(), 3);
    // one pass == repeated pass == brute hull
    fox::hive::Region r2x(g);
    std::vector<int> hull = r.member_ids();
    hull.insert(hull.end(), cl.violators.begin(), cl.violators.end());
    r2x.init(hull);
    ExpectTrue("hull convex (brute)", fox::hive::IsConvexBrute(g, r2x.member_ids()));
    fox::hive::ClosureResult again =
        fox::hive::ComputeClosure(g, r2x, {}, fox::hive::kClosureBudget);
    ExpectTrue("hull fixpoint", again.ok && again.violators.empty());
    Abc_NtkDelete(p);
}

void TestClosureLatchNotViolation()
{
    // r1 -> [latch] -> r2: no combinational path, absorbing both is convex.
    Abc_Ntk_t *p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(p);
    Abc_Obj_t *r1 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(r1, a); SetAnd(r1);
    Abc_Obj_t *bo = Abc_NtkAddLatch(p, r1, ABC_INIT_ZERO);
    Abc_Obj_t *r2 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(r2, bo); SetAnd(r2);
    Abc_Obj_t *po = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po, r2);
    fox::hive::CombGraph g(p);
    fox::hive::Region r(g);
    r.init({(int)Abc_ObjId(r1), (int)Abc_ObjId(r2)});
    fox::hive::ClosureResult cl =
        fox::hive::ComputeClosure(g, r, {}, fox::hive::kClosureBudget);
    ExpectTrue("latch path ok", cl.ok);
    ExpectEq("latch path no violators", (long)cl.violators.size(), 0);
    ExpectTrue("latch path convex (brute)", fox::hive::IsConvexBrute(g, r.member_ids()));
    Abc_NtkDelete(p);
}

void TestClosureBudget()
{
    // r1 -> c1..c10 -> r2 plus r1 -> r2: 10 violators; budget 3 -> rejected.
    Abc_Ntk_t *p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(p);
    Abc_Obj_t *r1 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(r1, a); SetAnd(r1);
    Abc_Obj_t *prev = r1;
    for (int i = 0; i < 10; ++i)
    {
        Abc_Obj_t *ci = Abc_NtkCreateNode(p);
        Abc_ObjAddFanin(ci, prev); SetAnd(ci);
        prev = ci;
    }
    Abc_Obj_t *r2 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(r2, r1); Abc_ObjAddFanin(r2, prev); SetAnd(r2);
    Abc_Obj_t *po = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po, r2);
    fox::hive::CombGraph g(p);
    fox::hive::Region r(g);
    r.init({(int)Abc_ObjId(r1), (int)Abc_ObjId(r2)});
    fox::hive::ClosureResult tight = fox::hive::ComputeClosure(g, r, {}, 3);
    ExpectTrue("budget exceeded -> not ok", !tight.ok);
    fox::hive::ClosureResult wide =
        fox::hive::ComputeClosure(g, r, {}, fox::hive::kClosureBudget);
    ExpectTrue("wide ok", wide.ok);
    ExpectEq("ten violators", (long)wide.violators.size(), 10);
    Abc_NtkDelete(p);
}
```

- [ ] **Step 2: Build to verify failure**

Run: `make release 2>&1 | grep error | head -3`
Expected: FAIL — `hive/convex.hpp: No such file or directory`.

- [ ] **Step 3: Write `src/hive/convex.hpp`**

```cpp
#ifndef HIVE_CONVEX_HPP
#define HIVE_CONVEX_HPP

#include <vector>

#include "hive/hive_graph.hpp"
#include "hive/region.hpp"

namespace fox::hive {

struct ClosureResult {
    bool ok = false;              // false: budget exceeded, reject the move
    std::vector<int> violators;   // sorted; valid only when ok
};

// Violators of R ∪ added: vertices outside the set that lie on a path from
// the set back into the set. Rank-band pruned per docs/hive-design.md 3.3;
// `budget` caps marked vertices per direction.
ClosureResult ComputeClosure(const CombGraph &g, const Region &r,
                             const std::vector<int> &added, int budget);

// Independent brute-force convexity check: full forward/backward
// reachability, no rank pruning, no budget. Test/verification only.
bool IsConvexBrute(const CombGraph &g, const std::vector<int> &memberIds);

} // namespace fox::hive

#endif // HIVE_CONVEX_HPP
```

- [ ] **Step 4: Write `src/hive/convex.cpp`**

```cpp
#include "hive/convex.hpp"

#include <algorithm>
#include <unordered_set>

namespace fox::hive {

ClosureResult ComputeClosure(const CombGraph &g, const Region &r,
                             const std::vector<int> &added, int budget)
{
    ClosureResult res;

    std::unordered_set<int> base;
    // Region::member_ids() copies; iterate members via the sorted view once.
    for (int id : r.member_ids())
        base.insert(id);
    for (int id : added)
        base.insert(id);
    if (base.empty())
    {
        res.ok = true;
        return res;
    }

    int rank_min = 0, rank_max = 0;
    bool first = true;
    for (int id : base)
    {
        const int rk = g.rank(id);
        if (first) { rank_min = rank_max = rk; first = false; }
        else
        {
            rank_min = std::min(rank_min, rk);
            rank_max = std::max(rank_max, rk);
        }
    }

    // forward: desc within the open band (rank strictly below rank_max)
    std::unordered_set<int> desc;
    std::vector<int> stack;
    for (int id : base)
        stack.push_back(id);
    while (!stack.empty())
    {
        const int v = stack.back();
        stack.pop_back();
        for (int s : g.succs(v))
        {
            if (g.rank(s) >= rank_max)
                continue;   // rank only grows; s can never reach back into base
            if (base.count(s) || desc.count(s))
                continue;
            desc.insert(s);
            if ((int)desc.size() > budget)
                return res;   // ok=false: reject the move (spec 3.3)
            stack.push_back(s);
        }
    }

    // backward: anc within the open band, collect desc ∩ anc
    std::unordered_set<int> anc;
    for (int id : base)
        stack.push_back(id);
    while (!stack.empty())
    {
        const int v = stack.back();
        stack.pop_back();
        for (int p : g.preds(v))
        {
            if (g.rank(p) <= rank_min)
                continue;
            if (base.count(p) || anc.count(p))
                continue;
            anc.insert(p);
            if ((int)anc.size() > budget)
                return res;
            stack.push_back(p);
        }
    }

    for (int v : desc)
        if (anc.count(v))
            res.violators.push_back(v);
    std::sort(res.violators.begin(), res.violators.end());
    res.ok = true;
    return res;
}

bool IsConvexBrute(const CombGraph &g, const std::vector<int> &memberIds)
{
    // Deliberately different code path: id-indexed flag arrays over the whole
    // graph, full reachability, no band, no budget.
    std::vector<char> member(g.max_id(), 0), desc(g.max_id(), 0), anc(g.max_id(), 0);
    for (int id : memberIds)
        member[id] = 1;

    std::vector<int> stack(memberIds.begin(), memberIds.end());
    while (!stack.empty())
    {
        const int v = stack.back();
        stack.pop_back();
        for (int s : g.succs(v))
            if (!desc[s])
            {
                desc[s] = 1;
                stack.push_back(s);
            }
    }
    stack.assign(memberIds.begin(), memberIds.end());
    while (!stack.empty())
    {
        const int v = stack.back();
        stack.pop_back();
        for (int p : g.preds(v))
            if (!anc[p])
            {
                anc[p] = 1;
                stack.push_back(p);
            }
    }
    for (int id : g.vertices())
        if (!member[id] && desc[id] && anc[id])
            return false;
    return true;
}

} // namespace fox::hive
```

- [ ] **Step 5: Build and run**

Run: `make release 2>&1 | tail -3 && ./release/test_hive`
Expected: `all hive tests passed`.

- [ ] **Step 6: Commit**

```bash
git add src/hive/convex.hpp src/hive/convex.cpp src/test_hive.cpp
git commit -m "hive: add rank-band convex closure and brute-force checker"
```

---

### Task 6: GrowFromSeed

**Files:**
- Create: `src/hive/hive_internal.hpp`; Modify: `src/hive/hive.cpp`
- Test: `src/test_hive.cpp`

**Interfaces:**
- Consumes: `CombGraph`, `Region`, `ComputeClosure`, constants from `hive.hpp`.
- Produces (in `src/hive/hive_internal.hpp`, exposed for tests per `csr3_internal.hpp` precedent):

```cpp
#ifndef HIVE_INTERNAL_HPP
#define HIVE_INTERNAL_HPP

#include "hive/hive.hpp"
#include "hive/hive_graph.hpp"

namespace fox::hive {

struct GrowResult {
    bool has_region = false;   // false: seed skipped (over caps) or invalid root
    RegionReport report;
};

// Grows one region from the MFFC of rootId (docs/hive-design.md 4).
GrowResult GrowFromSeed(const CombGraph &g, int rootId, const Config &cfg);

} // namespace fox::hive

#endif // HIVE_INTERNAL_HPP
```

- [ ] **Step 1: Add failing tests**

Add `#include "hive/hive_internal.hpp"` and `#include "hive/convex.hpp"` (if missing) plus:

```cpp
void TestMffcSeedConvexOutOne()
{
    // Reconvergent MFFC: a -> pp -> {q1,q2} -> root; root drives PO and ext e.
    // MFFC(root) = {root,q1,q2,pp}; must be convex with out == 1 (spec 4.1).
    Abc_Ntk_t *p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(p);
    Abc_Obj_t *pp = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(pp, a); SetAnd(pp);
    Abc_Obj_t *q1 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(q1, pp); SetAnd(q1);
    Abc_Obj_t *q2 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(q2, pp); SetAnd(q2);
    Abc_Obj_t *root = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(root, q1); Abc_ObjAddFanin(root, q2); SetAnd(root);
    Abc_Obj_t *e = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(e, root); SetAnd(e);
    Abc_Obj_t *po1 = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po1, root);
    Abc_Obj_t *po2 = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po2, e);
    fox::hive::CombGraph g(p);

    Vec_Ptr_t *vNodes = Vec_PtrAlloc(8);
    Abc_NodeMffcLabel(root, vNodes);
    std::vector<int> seed;
    Abc_Obj_t *pObj;
    int i;
    Vec_PtrForEachEntry(Abc_Obj_t *, vNodes, pObj, i)
        seed.push_back((int)Abc_ObjId(pObj));
    Vec_PtrFree(vNodes);
    ExpectEq("mffc size", (long)seed.size(), 4);
    ExpectTrue("mffc convex", fox::hive::IsConvexBrute(g, seed));
    fox::hive::Region r(g);
    r.init(seed);
    ExpectEq("mffc out==1", r.metrics(6).out, 1);
    Abc_NtkDelete(p);
}

void TestSeedOverCapsSkipped()
{
    // in > Imax: one node with 33 PI fanins under default -I 32 (spec 4.1)
    Abc_Ntk_t *p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *n = Abc_NtkCreateNode(p);
    for (int i = 0; i < 33; ++i)
        Abc_ObjAddFanin(n, Abc_NtkCreatePi(p));
    SetAnd(n);
    Abc_Obj_t *po = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po, n);
    fox::hive::CombGraph g(p);
    fox::hive::Config cfg;
    fox::hive::GrowResult gr = fox::hive::GrowFromSeed(g, (int)Abc_ObjId(n), cfg);
    ExpectTrue("oversized-in seed skipped", !gr.has_region);
    Abc_NtkDelete(p);

    // N > M: 6-node single-fanout chain, cfg.max_nodes = 4
    p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *prev = Abc_NtkCreatePi(p);
    Abc_Obj_t *last = nullptr;
    for (int i = 0; i < 6; ++i)
    {
        Abc_Obj_t *ni = Abc_NtkCreateNode(p);
        Abc_ObjAddFanin(ni, prev); SetAnd(ni);
        prev = ni;
        last = ni;
    }
    po = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po, last);
    fox::hive::CombGraph g2(p);
    fox::hive::Config small;
    small.max_nodes = 4;
    fox::hive::GrowResult gr2 = fox::hive::GrowFromSeed(g2, (int)Abc_ObjId(last), small);
    ExpectTrue("oversized-N seed skipped", !gr2.has_region);
    Abc_NtkDelete(p);
}

void TestBestPrefix()
{
    // c1 -> c2 (tight pair, Q=1.0), c2 -> j1,j2,j3 junk star, each j_i also
    // reads a fresh PI and drives its own PO. Growth must keep moving into
    // junk (feasible moves exist), Q declines; report = peak (spec 4.4).
    Abc_Ntk_t *p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *sh = Abc_NtkCreatePi(p);
    Abc_Obj_t *c1 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(c1, sh); SetAnd(c1);
    Abc_Obj_t *c2 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(c2, c1); SetAnd(c2);
    for (int i = 0; i < 3; ++i)
    {
        Abc_Obj_t *ji = Abc_NtkCreateNode(p);
        Abc_ObjAddFanin(ji, c2);
        Abc_ObjAddFanin(ji, Abc_NtkCreatePi(p));
        SetAnd(ji);
        Abc_Obj_t *poi = Abc_NtkCreatePo(p);
        Abc_ObjAddFanin(poi, ji);
    }
    fox::hive::CombGraph g(p);
    fox::hive::Config cfg;
    fox::hive::GrowResult gr = fox::hive::GrowFromSeed(g, (int)Abc_ObjId(c2), cfg);
    ExpectTrue("has region", gr.has_region);
    ExpectEq("peak members", (long)gr.report.member_ids.size(), 2);
    ExpectNear("peak q = 1.0", gr.report.q, 1.0);
    ExpectTrue("c1 in region", std::binary_search(gr.report.member_ids.begin(),
               gr.report.member_ids.end(), (int)Abc_ObjId(c1)));
    Abc_NtkDelete(p);
}

void TestTopLMissBaseline()
{
    // Regression baseline for spec 4.3's known loss: the best exact move is
    // ranked 5th+ optimistically and never evaluated.
    //   s: fanins sh(PI), d1..d4; fanouts x, e.
    //   d_i: fanin p_i (fresh PI); fanouts s and PO_i  (2 fanouts => not in MFFC(s))
    //   x: fanins s, k(fresh PI); fanout e.
    //   e: fanins s, x; fanout PO_e.
    // R={s}: in=5 {sh,d1..d4}, out=1 {s}, Q=1/6.
    // optimistic: d_i -> 2/7; x -> 2/8; e -> 2/8.  top4 = the four decoys.
    // exact e (closure pulls x): n=3, in=6 {sh,d1..4,k}, out=1 {e} -> 3/7 BEST.
    // With cfg.max_nodes=3: step0 absorbs a decoy (2/7), step1 absorbs another
    // (n=3, in: -d+p -> 5, out {s,d_i,d_j} = 3 -> 3/8), n==cap stops.
    // Expected report: q = 0.375, e and x never members.
    Abc_Ntk_t *p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *sh = Abc_NtkCreatePi(p);
    Abc_Obj_t *s = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(s, sh);
    std::vector<Abc_Obj_t *> decoys;
    for (int i = 0; i < 4; ++i)
    {
        Abc_Obj_t *di = Abc_NtkCreateNode(p);
        Abc_ObjAddFanin(di, Abc_NtkCreatePi(p)); SetAnd(di);
        Abc_ObjAddFanin(s, di);
        Abc_Obj_t *poi = Abc_NtkCreatePo(p);
        Abc_ObjAddFanin(poi, di);
        decoys.push_back(di);
    }
    SetAnd(s);
    Abc_Obj_t *x = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(x, s); Abc_ObjAddFanin(x, Abc_NtkCreatePi(p)); SetAnd(x);
    Abc_Obj_t *e = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(e, s); Abc_ObjAddFanin(e, x); SetAnd(e);
    Abc_Obj_t *poe = Abc_NtkCreatePo(p); Abc_ObjAddFanin(poe, e);

    fox::hive::CombGraph g(p);
    // manual: the e-move's exact value, via the same Region/Closure APIs
    fox::hive::Region manual(g);
    manual.init({(int)Abc_ObjId(s)});
    fox::hive::ClosureResult cl = fox::hive::ComputeClosure(
        g, manual, {(int)Abc_ObjId(e)}, fox::hive::kClosureBudget);
    ExpectTrue("manual closure ok", cl.ok);
    ExpectEq("manual closure pulls x", (long)cl.violators.size(), 1);
    manual.add((int)Abc_ObjId(e));
    for (int v : cl.violators)
        manual.add(v);
    const double manualQ = manual.metrics(6).q;   // 3/7

    fox::hive::Config cfg;
    cfg.max_nodes = 3;
    fox::hive::GrowResult gr = fox::hive::GrowFromSeed(g, (int)Abc_ObjId(s), cfg);
    ExpectTrue("has region", gr.has_region);
    ExpectNear("greedy q = 3/8", gr.report.q, 0.375);
    ExpectTrue("greedy misses the better move", gr.report.q < manualQ);
    ExpectTrue("e not in members", !std::binary_search(gr.report.member_ids.begin(),
               gr.report.member_ids.end(), (int)Abc_ObjId(e)));
    ExpectTrue("x not in members", !std::binary_search(gr.report.member_ids.begin(),
               gr.report.member_ids.end(), (int)Abc_ObjId(x)));
    Abc_NtkDelete(p);
}

void TestIntermediateCapRejected()
{
    // Baseline for spec 4.4: a move that transiently exceeds Imax is rejected
    // even though later absorptions could shrink the boundary again.
    // seed {m}: two vertex drivers u1,u2 (each 2 fresh-PI fanins, each also
    // drives a PO so it stays out of MFFC(m)). cfg.max_in = 2.
    // Absorbing u1: in = 2 - 1 + 2 = 3 > 2 -> infeasible; same for u2.
    Abc_Ntk_t *p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    std::vector<Abc_Obj_t *> us;
    for (int i = 0; i < 2; ++i)
    {
        Abc_Obj_t *ui = Abc_NtkCreateNode(p);
        Abc_ObjAddFanin(ui, Abc_NtkCreatePi(p));
        Abc_ObjAddFanin(ui, Abc_NtkCreatePi(p));
        SetAnd(ui);
        Abc_Obj_t *poi = Abc_NtkCreatePo(p);
        Abc_ObjAddFanin(poi, ui);
        us.push_back(ui);
    }
    Abc_Obj_t *m = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(m, us[0]); Abc_ObjAddFanin(m, us[1]); SetAnd(m);
    Abc_Obj_t *po = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po, m);
    fox::hive::CombGraph g(p);
    fox::hive::Config cfg;
    cfg.max_in = 2;
    fox::hive::GrowResult gr = fox::hive::GrowFromSeed(g, (int)Abc_ObjId(m), cfg);
    ExpectTrue("has region", gr.has_region);
    ExpectEq("stuck at seed", (long)gr.report.member_ids.size(), 1);
    Abc_NtkDelete(p);
}
```

- [ ] **Step 2: Build to verify failure**

Run: `make release 2>&1 | grep error | head -3`
Expected: FAIL — `hive/hive_internal.hpp: No such file or directory`.

- [ ] **Step 3: Create `src/hive/hive_internal.hpp`** (content in Interfaces above) **and implement in `src/hive/hive.cpp`**

Replace `src/hive/hive.cpp`'s includes and add the growth engine above `RunHive`:

```cpp
#include "hive/hive.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>
#include <vector>

#include "base/abc/abc.h"
#include "hive/convex.hpp"
#include "hive/hive_graph.hpp"
#include "hive/hive_internal.hpp"
#include "hive/region.hpp"

namespace fox::hive {

namespace {

bool CapsOk(const Metrics &m, const Config &cfg)
{
    return m.n <= cfg.max_nodes && m.in <= cfg.max_in && m.out <= cfg.max_out;
}

RegionReport MakeReport(const Region &r, const Metrics &m, int rootId)
{
    RegionReport rep;
    rep.root_id = rootId;
    rep.member_ids = r.member_ids();
    rep.n = m.n;
    rep.in = m.in;
    rep.out = m.out;
    rep.lb = m.lb;
    rep.gap = m.gap;
    rep.rank_min = m.rank_min;
    rep.rank_max = m.rank_max;
    rep.q = m.q;
    return rep;
}

} // namespace

GrowResult GrowFromSeed(const CombGraph &g, int rootId, const Config &cfg)
{
    GrowResult out;
    Abc_Obj_t *pRoot = Abc_NtkObj(g.ntk(), rootId);
    if (!pRoot || !Abc_ObjIsNode(pRoot))
        return out;

    Vec_Ptr_t *vNodes = Vec_PtrAlloc(16);
    Abc_NodeMffcLabel(pRoot, vNodes);
    std::vector<int> seedIds;
    Abc_Obj_t *pObj;
    int i;
    Vec_PtrForEachEntry(Abc_Obj_t *, vNodes, pObj, i)
        seedIds.push_back((int)Abc_ObjId(pObj));
    Vec_PtrFree(vNodes);
    if (seedIds.empty())
        return out;

    Region r(g);
    r.init(seedIds);
    Metrics m = r.metrics(cfg.lut_k);
    if (!CapsOk(m, cfg))
        return out;   // oversized seed: skip, no best-prefix (spec 4.1)

    Region bestR = r;
    Metrics bestM = m;
    int stall = 0;

    while (r.size() < cfg.max_nodes)
    {
        // candidates: entrance (external vertex drivers) + exit (sampled
        // fanouts of out members), deduped by id (spec 4.2, 4.6)
        std::vector<int> cands = r.entrance_candidates();
        {
            std::unordered_set<int> seen(cands.begin(), cands.end());
            for (int om : r.out_members())
            {
                int taken = 0;
                for (int s : g.succs(om))
                {
                    if (taken >= kFanoutCap)
                        break;
                    if (r.contains(s) || seen.count(s))
                        continue;
                    seen.insert(s);
                    cands.push_back(s);
                    ++taken;
                }
            }
        }
        if (cands.empty())
            break;

        // optimistic pass: empty-closure trial per candidate (spec 4.3)
        struct Scored
        {
            int id;
            double q;
        };
        std::vector<Scored> scored;
        scored.reserve(cands.size());
        for (int c : cands)
        {
            Region t = r;
            t.add(c);
            scored.push_back({c, t.metrics(cfg.lut_k).q});
        }
        const int topl = std::min<int>(kTopL, (int)scored.size());
        std::nth_element(scored.begin(), scored.begin() + topl, scored.end(),
                         [](const Scored &a, const Scored &b) { return a.q > b.q; });

        // exact pass on the top L
        bool moved = false;
        Region nextR(g);
        Metrics nextM;
        double nextQ = -1.0;
        for (int k = 0; k < topl; ++k)
        {
            ClosureResult cl = ComputeClosure(g, r, {scored[k].id}, kClosureBudget);
            if (!cl.ok)
                continue;   // budget exceeded: reject this move (spec 3.3)
            Region t = r;
            t.add(scored[k].id);
            for (int v : cl.violators)
                t.add(v);
            Metrics tm = t.metrics(cfg.lut_k);
            if (!CapsOk(tm, cfg))
                continue;   // caps bind on intermediate states (spec 4.4)
            if (tm.q > nextQ)
            {
                nextQ = tm.q;
                nextR = std::move(t);
                nextM = tm;
                moved = true;
            }
        }
        if (!moved)
            break;

        r = std::move(nextR);
        m = nextM;
        if (m.q > bestM.q)
        {
            bestR = r;
            bestM = m;
            stall = 0;
        }
        else if (++stall >= kStall)
            break;
    }

    out.has_region = true;
    out.report = MakeReport(bestR, bestM, rootId);
    return out;
}
```

(Keep the existing `RunHive`/`ApplyHive` stubs below this, unchanged for now.)

- [ ] **Step 4: Build and run**

Run: `make release 2>&1 | tail -3 && ./release/test_hive`
Expected: `all hive tests passed`. If `TestTopLMissBaseline` fails on the exact `0.375`, print the actual trajectory by temporarily adding a `printf` in the test (not the library); the constructed arithmetic above is exact, so a mismatch means an implementation bug in candidate enumeration or closure, not a tuning problem.

- [ ] **Step 5: Commit**

```bash
git add src/hive/hive_internal.hpp src/hive/hive.cpp src/test_hive.cpp
git commit -m "hive: add greedy growth with best-prefix and known-loss baselines"
```

---

### Task 7: RunHive pipeline — seeds, dedup, Top-N

**Files:**
- Modify: `src/hive/hive.cpp` (replace the `RunHive` stub)
- Test: `src/test_hive.cpp`

**Interfaces:**
- Consumes: `GrowFromSeed`. Produces the final `RunHive` behavior: seeds = internal nodes sorted by `Abc_NodeMffcSize` descending (ties by id ascending), top `num_seeds` (0 = all); grow each; sort candidates by Q descending; greedy Jaccard dedup at `kJaccardPct`; truncate to `num_regions`.

- [ ] **Step 1: Add failing tests**

```cpp
void TestRunHiveDedupAndConsistency()
{
    // Two seeds inside one tight 3-cluster grow to overlapping regions;
    // dedup must keep one (spec 4.5). Cluster: sh->g1->g2->g3->PO.
    Abc_Ntk_t *p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *sh = Abc_NtkCreatePi(p);
    Abc_Obj_t *g1 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(g1, sh); SetAnd(g1);
    Abc_Obj_t *g2 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(g2, g1); SetAnd(g2);
    Abc_Obj_t *g3 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(g3, g2); SetAnd(g3);
    Abc_Obj_t *po = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po, g3);
    fox::hive::Config cfg;
    fox::hive::Result res = fox::hive::RunHive(p, cfg);
    ExpectTrue("ok", res.ok);
    ExpectEq("dedup to one region", (long)res.regions.size(), 1);
    // report fields match an independent recompute
    fox::hive::CombGraph g(p);
    for (const fox::hive::RegionReport &rep : res.regions)
    {
        fox::hive::Region r(g);
        r.init(rep.member_ids);
        fox::hive::Metrics m = r.recompute(cfg.lut_k);
        ExpectEq("rep n", rep.n, m.n);
        ExpectEq("rep in", rep.in, m.in);
        ExpectEq("rep out", rep.out, m.out);
        ExpectEq("rep lb", rep.lb, m.lb);
        ExpectNear("rep q", rep.q, m.q);
        ExpectTrue("rep convex", fox::hive::IsConvexBrute(g, rep.member_ids));
    }
    Abc_NtkDelete(p);
}

void TestRunHiveSeedCapAndSort()
{
    // num_seeds=1 must still work; regions sorted Q-descending
    Diamond d = BuildDiamond();
    fox::hive::Config cfg;
    cfg.num_seeds = 1;
    fox::hive::Result res = fox::hive::RunHive(d.ntk, cfg);
    ExpectTrue("ok with 1 seed", res.ok);
    for (size_t i = 1; i < res.regions.size(); ++i)
        ExpectTrue("q descending", res.regions[i - 1].q >= res.regions[i].q);
    Abc_NtkDelete(d.ntk);
}
```

- [ ] **Step 2: Run to verify failure**

Run: `make release 2>&1 | tail -3 && ./release/test_hive 2>&1 | head -5`
Expected: FAIL — stub `RunHive` returns zero regions.

- [ ] **Step 3: Replace the `RunHive` stub in `src/hive/hive.cpp`**

```cpp
namespace {

int JaccardPct(const std::vector<int> &a, const std::vector<int> &b)
{
    // both sorted ascending
    size_t i = 0, j = 0;
    int inter = 0;
    while (i < a.size() && j < b.size())
    {
        if (a[i] < b[j])
            ++i;
        else if (a[i] > b[j])
            ++j;
        else
        {
            ++inter;
            ++i;
            ++j;
        }
    }
    const int uni = (int)a.size() + (int)b.size() - inter;
    return uni > 0 ? (int)(100LL * inter / uni) : 0;
}

} // namespace

Result RunHive(Abc_Ntk_t *pNtk, const Config &cfg)
{
    Result res;
    if (!pNtk || !Abc_NtkIsLogic(pNtk))
        return res;
    CombGraph g(pNtk);
    if (!g.acyclic())
    {
        printf("hive: combinational loop detected\n");
        return res;
    }
    res.ok = true;
    if (g.vertices().empty())
        return res;   // no internal nodes: normal empty result (spec 6)

    // seeds: MFFC size descending, ties by id, top num_seeds (spec 4.1)
    std::vector<std::pair<int, int>> sized;   // (mffc_size, id)
    sized.reserve(g.vertices().size());
    for (int id : g.vertices())
        sized.push_back({Abc_NodeMffcSize(Abc_NtkObj(pNtk, id)), id});
    std::sort(sized.begin(), sized.end(), [](const auto &a, const auto &b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    const int nseeds = cfg.num_seeds == 0
                           ? (int)sized.size()
                           : std::min<int>(cfg.num_seeds, (int)sized.size());

    std::vector<RegionReport> cands;
    for (int k = 0; k < nseeds; ++k)
    {
        GrowResult gr = GrowFromSeed(g, sized[k].second, cfg);
        if (gr.has_region)
            cands.push_back(std::move(gr.report));
    }
    std::sort(cands.begin(), cands.end(),
              [](const RegionReport &a, const RegionReport &b) { return a.q > b.q; });

    for (RegionReport &c : cands)
    {
        if ((int)res.regions.size() >= cfg.num_regions)
            break;
        bool dup = false;
        for (const RegionReport &acc : res.regions)
            if (JaccardPct(c.member_ids, acc.member_ids) > kJaccardPct)
            {
                dup = true;
                break;
            }
        if (!dup)
            res.regions.push_back(std::move(c));
    }
    return res;
}
```

- [ ] **Step 4: Build and run**

Run: `make release 2>&1 | tail -3 && ./release/test_hive`
Expected: `all hive tests passed`.

- [ ] **Step 5: Commit**

```bash
git add src/hive/hive.cpp src/test_hive.cpp
git commit -m "hive: add RunHive pipeline with MFFC seeds and Jaccard dedup"
```

---

### Task 8: Report printing, ApplyHive, command registration

**Files:**
- Modify: `src/hive/hive.cpp` (`ApplyHive`), `src/main.cpp`

**Interfaces:**
- Consumes: `RunHive`. Produces: the `hive` FoxSYN command with the spec §6 flag set and §7 report format.

- [ ] **Step 1: Replace `ApplyHive` in `src/hive/hive.cpp`**

```cpp
bool ApplyHive(Abc_Frame_t *pAbc, const Config &cfg)
{
    Abc_Ntk_t *pNtk = Abc_FrameReadNtk(pAbc);
    if (!pNtk)
    {
        printf("hive: network is null\n");
        return false;
    }
    if (!Abc_NtkIsLogic(pNtk))
    {
        printf("hive: network must be logic (run if -K first)\n");
        return false;
    }

    const int nNodes = Abc_NtkNodeNum(pNtk);
    const int nSeeds = cfg.num_seeds == 0 ? nNodes : std::min(cfg.num_seeds, nNodes);
    Result res = RunHive(pNtk, cfg);
    if (!res.ok)
        return false;

    printf("hive: %d nodes, K=%d, %d seeds\n", nNodes, cfg.lut_k, nSeeds);
    printf("hive: gap is against LB under model M1-M3 (structural-pin-preserving, LUT-only);\n");
    printf("      it is NOT a lower bound for functional resynthesis -- see docs/hive-design.md 2.5\n");
    if (res.regions.empty())
    {
        printf("hive: 0 regions\n");
        return true;
    }

    printf("  #  root          N   in  out      Q    LB   gap  rank\n");
    std::unordered_set<int> distinct;
    for (size_t idx = 0; idx < res.regions.size(); ++idx)
    {
        const RegionReport &r = res.regions[idx];
        printf("%3zu  %-12s %4d %4d %4d %6.2f %5d %5d  %d..%d\n",
               idx, Abc_ObjName(Abc_NtkObj(pNtk, r.root_id)),
               r.n, r.in, r.out, r.q, r.lb, r.gap, r.rank_min, r.rank_max);
        distinct.insert(r.member_ids.begin(), r.member_ids.end());
        if (cfg.verbose)
        {
            CombGraph g(pNtk);
            Region reg(g);
            reg.init(r.member_ids);
            printf("     members:");
            for (int id : r.member_ids)
                printf(" %d", id);
            printf("\n     in:");
            for (int id : reg.in_objects())
                printf(" %d", id);
            printf("\n     out:");
            for (int id : reg.out_members())
                printf(" %d", id);
            printf("\n");
        }
    }
    printf("hive: %zu regions, %zu distinct nodes (%.1f%% of netlist), Q in [%.2f, %.2f]\n",
           res.regions.size(), distinct.size(),
           nNodes > 0 ? 100.0 * (double)distinct.size() / nNodes : 0.0,
           res.regions.back().q, res.regions.front().q);
    return true;
}
```

Note: the verbose branch rebuilds a `CombGraph` per region purely for boundary listings — acceptable for a verbose path; hoist it above the loop if it bothers you during implementation (one-line change).

- [ ] **Step 2: Add `Hive_Command` to `src/main.cpp`**

Include (after the pst include): `#include "hive/hive.hpp"`.

Add before `struct CmdRegister` (style matches `Pdecomp_Command`, `src/main.cpp:1059`):

```cpp
int Hive_Command(Abc_Frame_t *pAbc, int argc, char **argv)
{
    fox::hive::Config cfg;

    if (argc > 1 && !strcmp(argv[1], "-h"))
        goto usage;

    for (int i = 1; i != argc; ++i)
    {
        if (argv[i][0] != '-')
        {
            std::cout << "hive: unexpected argument " << argv[i] << "\n";
            goto usage;
        }
        const char arg = *(argv[i] + 1);
        switch (arg)
        {
        case 'N':
            if (i + 1 >= argc) { printf("hive: -N requires a number\n"); return 1; }
            cfg.num_regions = std::atoi(argv[++i]);
            if (cfg.num_regions < 1) { printf("hive: -N must be >= 1\n"); return 1; }
            break;
        case 'M':
            if (i + 1 >= argc) { printf("hive: -M requires a number\n"); return 1; }
            cfg.max_nodes = std::atoi(argv[++i]);
            if (cfg.max_nodes < 1) { printf("hive: -M must be >= 1\n"); return 1; }
            break;
        case 'I':
            if (i + 1 >= argc) { printf("hive: -I requires a number\n"); return 1; }
            cfg.max_in = std::atoi(argv[++i]);
            if (cfg.max_in < 1) { printf("hive: -I must be >= 1\n"); return 1; }
            break;
        case 'O':
            if (i + 1 >= argc) { printf("hive: -O requires a number\n"); return 1; }
            cfg.max_out = std::atoi(argv[++i]);
            if (cfg.max_out < 1) { printf("hive: -O must be >= 1\n"); return 1; }
            break;
        case 'K':
            if (i + 1 >= argc) { printf("hive: -K requires a number\n"); return 1; }
            cfg.lut_k = std::atoi(argv[++i]);
            if (cfg.lut_k < 2 || cfg.lut_k > 16) { printf("hive: -K must be 2-16\n"); return 1; }
            break;
        case 'S':
            if (i + 1 >= argc) { printf("hive: -S requires a number\n"); return 1; }
            cfg.num_seeds = std::atoi(argv[++i]);
            if (cfg.num_seeds < 0) { printf("hive: -S must be >= 0\n"); return 1; }
            break;
        case 'v':
            cfg.verbose = true;
            break;
        case 'h':
            goto usage;
        default:
            std::cout << "hive: unknown argument -" << arg << "\n";
            goto usage;
        }
    }

    return fox::hive::ApplyHive(pAbc, cfg) ? 0 : 1;

usage:
    Abc_Print(-2, "usage: hive [-N num] [-M num] [-I num] [-O num] [-K num] [-S num] [-v]\n");
    Abc_Print(-2, "\t           find dense low-boundary logic clusters (read-only report)\n");
    Abc_Print(-2, "\t-N num  : number of regions to report (>=1) [default = 20]\n");
    Abc_Print(-2, "\t-M num  : region node cap (>=1) [default = 64]\n");
    Abc_Print(-2, "\t-I num  : region input cap (>=1) [default = 32]\n");
    Abc_Print(-2, "\t-O num  : region output cap (>=1) [default = 8]\n");
    Abc_Print(-2, "\t-K num  : LUT width for the lower bound (2-16) [default = 6]\n");
    Abc_Print(-2, "\t-S num  : seed cap, 0 = all nodes [default = 2000]\n");
    Abc_Print(-2, "\t-v      : print member and boundary object ids\n");
    Abc_Print(-2, "\n");
    return 1;
}
```

Registration inside `CmdRegister` (after the pdecomp line; last arg 0 — hive never changes the network):

```cpp
Cmd_CommandAdd(Abc_FrameGetGlobalFrame(), "FoxSYN", "hive", Hive_Command, 0);
```

- [ ] **Step 3: Build and smoke-test manually**

```bash
make release 2>&1 | tail -3
./release/FoxSYN -c "read regression/SimpleCircuits/mcnc/alu4.v; if -K 6; hive"
./release/FoxSYN -c "read regression/SimpleCircuits/mcnc/alu4.v; if -K 6; hive -N 5 -v" | head -30
./release/FoxSYN -c "read regression/SimpleCircuits/mcnc/alu4.v; strash; hive"
```

Expected: first two print a plausible report (regions with `Q > 1`, caps respected); the `strash` run prints `hive: network must be logic` and fails. Also `./release/test_hive` still passes.

- [ ] **Step 4: Commit**

```bash
git add src/hive/hive.cpp src/main.cpp
git commit -m "hive: add report printing and register the hive command"
```

---

### Task 9: Integration + cross-validation on real netlists

**Files:**
- Modify: `src/test_hive.cpp`

**Interfaces:**
- Consumes everything. Produces the spec §8 integration gate: for every reported region on a real netlist — brute-force convexity, caps, `LB ≤ N`, member validity, pairwise Jaccard, report-vs-recompute equality, and byte-level read-only verification.

- [ ] **Step 1: Add the integration runner**

```cpp
// Snapshot of the mutable per-object state hive must not change (spec 6).
struct ObjSnapshot {
    unsigned type;
    int nFanins, nFanouts;
    unsigned level;
    std::vector<int> fanins;
};

std::vector<ObjSnapshot> SnapshotNtk(Abc_Ntk_t *pNtk)
{
    std::vector<ObjSnapshot> snap(Abc_NtkObjNumMax(pNtk));
    Abc_Obj_t *pObj;
    int i;
    Abc_NtkForEachObj(pNtk, pObj, i)
    {
        ObjSnapshot &s = snap[i];
        s.type = pObj->Type;
        s.nFanins = Abc_ObjFaninNum(pObj);
        s.nFanouts = Abc_ObjFanoutNum(pObj);
        s.level = pObj->Level;
        Abc_Obj_t *pFanin;
        int k;
        Abc_ObjForEachFanin(pObj, pFanin, k)
            s.fanins.push_back((int)Abc_ObjId(pFanin));
    }
    return snap;
}

int RunCircuitFile(const char *path)
{
    Abc_Ntk_t *pRead = Io_Read((char *)path, Io_ReadFileType((char *)path), 1, 0);
    if (!pRead)
    {
        std::fprintf(stderr, "FAIL cannot read %s\n", path);
        return ++g_fail, 1;
    }
    Abc_Ntk_t *pNtk = Abc_NtkIsNetlist(pRead) ? Abc_NtkToLogic(pRead) : pRead;
    if (pNtk != pRead)
        Abc_NtkDelete(pRead);

    std::vector<ObjSnapshot> before = SnapshotNtk(pNtk);
    fox::hive::Config cfg;
    fox::hive::Result res = fox::hive::RunHive(pNtk, cfg);
    ExpectTrue("integration ok", res.ok);

    fox::hive::CombGraph g(pNtk);
    for (size_t a = 0; a < res.regions.size(); ++a)
    {
        const fox::hive::RegionReport &r = res.regions[a];
        ExpectTrue("convex (brute)", fox::hive::IsConvexBrute(g, r.member_ids));
        ExpectTrue("cap N", r.n <= cfg.max_nodes);
        ExpectTrue("cap in", r.in <= cfg.max_in);
        ExpectTrue("cap out", r.out <= cfg.max_out);
        ExpectTrue("lb <= n", r.lb <= r.n);
        for (int id : r.member_ids)
        {
            Abc_Obj_t *pObj = Abc_NtkObj(pNtk, id);
            ExpectTrue("member is node", pObj && Abc_ObjIsNode(pObj));
        }
        fox::hive::Region reg(g);
        reg.init(r.member_ids);
        fox::hive::Metrics m = reg.recompute(cfg.lut_k);
        ExpectEq("int n", r.n, m.n);
        ExpectEq("int in", r.in, m.in);
        ExpectEq("int out", r.out, m.out);
        ExpectEq("int lb", r.lb, m.lb);
        ExpectEq("int rank_min", r.rank_min, m.rank_min);
        ExpectEq("int rank_max", r.rank_max, m.rank_max);
        ExpectNear("int q", r.q, m.q);
        for (size_t b = 0; b < a; ++b)
        {
            // reuse the same two-pointer Jaccard the library uses, inline here
            const std::vector<int> &x = r.member_ids;
            const std::vector<int> &y = res.regions[b].member_ids;
            size_t ii = 0, jj = 0;
            int inter = 0;
            while (ii < x.size() && jj < y.size())
            {
                if (x[ii] < y[jj]) ++ii;
                else if (x[ii] > y[jj]) ++jj;
                else { ++inter; ++ii; ++jj; }
            }
            const int uni = (int)x.size() + (int)y.size() - inter;
            ExpectTrue("pairwise jaccard <= threshold",
                       uni == 0 || 100LL * inter / uni <= fox::hive::kJaccardPct);
        }
    }

    // read-only: structure and Level unchanged object-by-object
    std::vector<ObjSnapshot> after = SnapshotNtk(pNtk);
    ExpectEq("obj count unchanged", (long)after.size(), (long)before.size());
    for (size_t i = 0; i < before.size() && i < after.size(); ++i)
    {
        ExpectTrue("type unchanged", before[i].type == after[i].type);
        ExpectTrue("fanins unchanged", before[i].fanins == after[i].fanins);
        ExpectEq("fanout count restored", after[i].nFanouts, before[i].nFanouts);
        ExpectTrue("Level unchanged", before[i].level == after[i].level);
    }

    std::printf("integration %s: %zu regions\n", path, res.regions.size());
    Abc_NtkDelete(pNtk);
    return 0;
}
```

Change `main` to accept file arguments (pattern of `test_fmpart.cpp`):

```cpp
int main(int argc, char **argv)
{
    Abc_Start();
    if (argc > 1) {
        for (int i = 1; i < argc; ++i)
            RunCircuitFile(argv[i]);
        const int r = g_fail == 0 ? 0 : 1;
        if (g_fail == 0) std::printf("all hive integration checks passed\n");
        Abc_Stop();
        return r;
    }
    // ... existing unit test calls unchanged ...
}
```

Implementation note: check the exact `Io_Read` signature in `src/abc/src/base/io/ioAbc.h` before writing this — if the vendored ABC has the 3-argument form, drop the trailing `0`.

- [ ] **Step 2: Build and run both modes**

```bash
make release 2>&1 | tail -3
./release/test_hive
./release/test_hive regression/SimpleCircuits/mcnc/alu4.v
ls regression/SimpleCircuits/EPFL | head -5
```

Expected: both runs pass. Pick two files from the EPFL listing and run them through `./release/test_hive` as well; all assertions must hold.

- [ ] **Step 3: Commit**

```bash
git add src/test_hive.cpp
git commit -m "hive: add integration cross-validation with brute-force convexity"
```

---

### Task 10: User docs and spec status

**Files:**
- Create: `docs/hive.md`
- Modify: `docs/hive-design.md` (status line only)

- [ ] **Step 1: Write `docs/hive.md`**

Follow `docs/hpart.md`'s structure. Content requirements (write actual prose, in Chinese like the spec or English like hpart.md — match hpart.md's English):

- Purpose: one paragraph — read-only discovery of convex, narrow-boundary, dense-interior regions; report only, no netlist changes, no Pdb.
- Command line: the §6 table (flags, ranges, defaults) transcribed.
- Metrics: definitions of `N/in/out/Q/LB/gap/rank` with the M1-M3 caveat sentence and an explicit pointer to `docs/hive-design.md` §2.5.
- Report format: the sample block from spec §7.
- Preconditions and read-only guarantees (spec §6).
- Related files list: `src/hive/*`, `src/test_hive.cpp`, `docs/hive-design.md`.

- [ ] **Step 2: Update the spec status line**

In `docs/hive-design.md` change `**状态**：设计已批准，待写实现计划` to `**状态**：已实现（见 docs/hive.md 与 src/hive/）`.

- [ ] **Step 3: Final verification**

```bash
make release 2>&1 | tail -3
./release/test_hive
./release/test_hive regression/SimpleCircuits/mcnc/alu4.v
./release/FoxSYN -c "read regression/SimpleCircuits/mcnc/alu4.v; if -K 6; hive"
```

All green.

- [ ] **Step 4: Commit**

```bash
git add docs/hive.md docs/hive-design.md
git commit -m "hive: add command documentation and mark spec implemented"
```

---

## Plan self-review notes

- **Spec coverage:** §2.1-2.5 → Tasks 3/4 (const1 ordinary, dedup counting, Q, LB three terms + clamp, functional counterexample); §3.1-3.3 → Task 5 (one-pass closure, band on own rank, budget-reject); §4.1-4.6 → Task 6 (MFFC seed + convexity/out==1 test, oversized-seed skip, entrance+exit moves, top-L baseline, best-prefix, intermediate-cap baseline, fanout sampling) and Task 7 (seed ordering, Jaccard, Top-N); §5/5.1 → Tasks 1/2 (module layout, CombGraph spec incl. latch BI/BO and rank); §6 → Tasks 1/8 (flags, ranges, preconditions, read-only); §7 → Task 8; §8 → Tasks 2-9 (unit + integration + brute checker + read-only snapshot); §9 needs no code.
- **Known deviations from spec §8's test list, both deliberate:** (1) exhaustive small-DAG LB enumeration reduced to three hand-verified-optimal cases (Task 4 commit message records this); (2) the "rank-band vs full check find the same violator set" assertion is realized as one-pass == brute-hull fixpoint equality (Task 5) rather than a separate band-disabled closure variant — `IsConvexBrute` is the independent path the spec actually requires.
- **Type consistency:** `Region::metrics/recompute` both return `Metrics`; `RegionReport` fields match `MakeReport`; `GrowFromSeed` consumes `Config` by const ref; constants referenced by tests come from `hive.hpp` only.
