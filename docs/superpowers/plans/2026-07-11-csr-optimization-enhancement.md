# CSR Optimization Enhancement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Improve CSR cut-edge reduction while preserving entry hop, limiting cumulative positive node growth to 2%, limiting cut-net to 150% of entry, keeping default runtime within 5×, and producing deterministic results.

**Architecture:** Keep the existing Phase 0/1/2 order, but add a shared `OptimizationState`, exact transaction checks, a standalone incremental `HopState`, bounded compound searches, and an optional deterministic multi-trajectory wrapper. The current `csr.cpp` remains the phase implementation file; reusable state and hop logic move into focused internal files so they can be unit-tested without exposing them as public API.

**Tech Stack:** C++23, Berkeley ABC logic networks and MFS APIs, CMake, Python 3 regression scripts, ABC `cec`, AddressSanitizer/UBSan.

---

## File map

- Modify `src/csr/csr.hpp`: public `Config`, `ApplyCsr(Abc_Frame_t *, ...)`, and user-visible defaults.
- Create `src/csr/csr_internal.hpp`: internal metrics, limits, budgets, trajectory, relocation, resub, and replication declarations used by `csr.cpp` and tests.
- Create `src/csr/csr_state.cpp`: entry-state capture, metric recomputation, PDB metadata restore, deterministic comparison, and search-budget accounting.
- Create `src/csr/csr_hop.cpp`: exact incremental hop propagation and rollback.
- Modify `src/csr/csr.cpp`: Phase 0/1/2 enhancements and trajectory orchestration.
- Modify `src/csr/CMakeLists.txt`: compile the new internal source files.
- Create `src/test_csr.cpp`: focused tests for state, determinism, relocation, hop propagation, resub scoring, and replication clusters.
- Modify `src/CMakeLists.txt`: build `test_csr`.
- Modify `src/main.cpp`: parse `-T` and pass `Abc_Frame_t *` to CSR.
- Modify `scripts/run_csr_regression.py`: hard-constraint checks, exact-repeat mode, richer phase statistics, baseline comparison, and runtime reporting.
- Modify `docs/csr.md`: update algorithm, CLI, constraints, ownership, and validation results.

## Task 0: Capture the current CSR baseline binary

**Files:**
- No repository changes.

- [ ] **Step 1: Build the unmodified baseline**

Run before editing any source file:

```bash
rtk make release
```

Expected: `FoxSYN`, `test_hop`, and `test_cpr` build successfully.

- [ ] **Step 2: Preserve the baseline executable outside the worktree**

```bash
rtk cp release/FoxSYN /tmp/FoxSYN-csr-baseline-799702c
rtk sha256sum /tmp/FoxSYN-csr-baseline-799702c
```

Expected: the copied binary exists and prints one SHA-256 digest. Keep this path unchanged through Task 12.

- [ ] **Step 3: Record a frozen-partition smoke result**

```bash
rtk /tmp/FoxSYN-csr-baseline-799702c -c "read regression/SimpleCircuits/mcnc/alu4.v; st; if -K 6; hpart -N 4 --load-part regression/parts_n4_flat/alu4.part; csr -v; ps"
```

Expected: CSR succeeds and prints the baseline cut-edge, cut-net, hop, node count, phase counts, and runtime for later comparison.

## Task 1: Add the CSR internal test target and entry-state types

**Files:**
- Create: `src/csr/csr_internal.hpp`
- Create: `src/csr/csr_state.cpp`
- Create: `src/test_csr.cpp`
- Modify: `src/csr/CMakeLists.txt`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Write failing tests for entry limits and PDB scalar preservation**

Create `src/test_csr.cpp` with a tiny logic network and these initial tests:

```cpp
#include <cstdio>

#include "base/abc/abc.h"
#include "csr/csr.hpp"
#include "csr/csr_internal.hpp"

namespace {

bool ExpectEqual(const char *label, int actual, int expected)
{
    if (actual == expected)
        return true;
    std::fprintf(stderr, "%s: expected %d, got %d\n", label, expected, actual);
    return false;
}

void SetNodeFunction(Abc_Obj_t *pNode)
{
    auto *pMan = static_cast<Mem_Flex_t *>(pNode->pNtk->pManFunc);
    if (Abc_ObjFaninNum(pNode) == 1)
        pNode->pData = Abc_SopCreateBuf(pMan);
    else
        pNode->pData = Abc_SopCreateAnd(pMan, Abc_ObjFaninNum(pNode), nullptr);
}

struct StateTestNtk {
    Abc_Ntk_t *pNtk = nullptr;
    Abc_Obj_t *pPi0 = nullptr;
    Abc_Obj_t *pPi1 = nullptr;
    Abc_Obj_t *pNode = nullptr;
    Abc_Obj_t *pPo = nullptr;
};

StateTestNtk CreateStateTestNtk()
{
    StateTestNtk t;
    t.pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    t.pPi0 = Abc_NtkCreatePi(t.pNtk);
    t.pPi1 = Abc_NtkCreatePi(t.pNtk);
    t.pNode = Abc_NtkCreateNode(t.pNtk);
    t.pPo = Abc_NtkCreatePo(t.pNtk);
    Abc_ObjAddFanin(t.pNode, t.pPi0);
    Abc_ObjAddFanin(t.pNode, t.pPi1);
    Abc_ObjAddFanin(t.pPo, t.pNode);
    SetNodeFunction(t.pNode);
    Abc_ObjSetPartId(t.pPi0, 0);
    Abc_ObjSetPartId(t.pPi1, 1);
    Abc_ObjSetPartId(t.pNode, 1);
    const int cut = Abc_NtkComputeCutSize(t.pNtk);
    const int hop = Abc_NtkComputeHopNum(t.pNtk);
    Abc_NtkSetPartStats(t.pNtk, 2, cut, hop);
    t.pNtk->pPdb->set_balance_pct(17);
    return t;
}

bool TestCaptureEntryLimitsBeforeDup()
{
    StateTestNtk t = CreateStateTestNtk();
    fox::csr::Config cfg;
    cfg.replicate_growth_pct = 2;
    auto limits = fox::csr::detail::CaptureEntryLimits(t.pNtk, cfg);
    Abc_Ntk_t *pDup = Abc_NtkDup(t.pNtk);

    bool ok = true;
    ok &= ExpectEqual("captured num parts", limits.num_parts, 2);
    ok &= ExpectEqual("captured balance", limits.balance_pct, 17);
    ok &= ExpectEqual("dup invalidates balance", pDup->pPdb->balance_pct(), -1);

    fox::csr::detail::RestorePdbMetadata(pDup, limits);
    ok &= ExpectEqual("restored balance", pDup->pPdb->balance_pct(), 17);
    ok &= ExpectEqual("restored num parts", pDup->pPdb->num_parts(), 2);

    Abc_NtkDelete(pDup);
    Abc_NtkDelete(t.pNtk);
    return ok;
}

} // namespace

int main()
{
    return TestCaptureEntryLimitsBeforeDup() ? 0 : 1;
}
```

- [ ] **Step 2: Wire the test target and verify the test fails**

Add to `src/CMakeLists.txt`:

```cmake
add_executable(test_csr "test_csr.cpp")
target_link_libraries(test_csr PRIVATE
    libabc
    timer
    cpr
    csr
)
```

Change `src/csr/CMakeLists.txt` to:

```cmake
add_library(csr
    csr.cpp
    csr_state.cpp
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-fexceptions)

target_link_libraries(csr PRIVATE libabc timer cpr)
target_include_directories(csr PUBLIC ${CMAKE_SOURCE_DIR}/abc/src ${CMAKE_SOURCE_DIR})
```

Run:

```bash
rtk make release
```

Expected: compilation fails because `csr_internal.hpp`, `CaptureEntryLimits`, and `RestorePdbMetadata` are not implemented.

- [ ] **Step 3: Add the minimal internal state API**

Create `src/csr/csr_internal.hpp`:

```cpp
#ifndef CSR_INTERNAL_HPP
#define CSR_INTERNAL_HPP

#include <optional>
#include <tuple>
#include <vector>

#include "csr.hpp"
#include "base/abc/abc.h"

namespace fox::csr::detail {

struct Metrics {
    int cut_edges = 0;
    int cut_nets = 0;
    int hop = 0;
    int nodes = 0;
};

struct EntryLimits {
    int num_parts = 0;
    int balance_pct = 2;
    int hop_limit = 0;
    int node_limit = 0;
    int growth_budget = 0;
    int cutnet_limit = 0;
};

Metrics ComputeMetrics(Abc_Ntk_t *pNtk);
EntryLimits CaptureEntryLimits(Abc_Ntk_t *pNtk, const Config &cfg);
void RestorePdbMetadata(Abc_Ntk_t *pNtk, const EntryLimits &limits);

} // namespace fox::csr::detail

#endif
```

Implement `src/csr/csr_state.cpp`:

```cpp
#include "csr_internal.hpp"

#include <algorithm>

namespace fox::csr::detail {

Metrics ComputeMetrics(Abc_Ntk_t *pNtk)
{
    return {
        ComputeCutEdgeCount(pNtk),
        Abc_NtkComputeCutSize(pNtk),
        Abc_NtkComputeHopNum(pNtk),
        Abc_NtkNodeNum(pNtk),
    };
}

EntryLimits CaptureEntryLimits(Abc_Ntk_t *pNtk, const Config &cfg)
{
    Metrics metrics = ComputeMetrics(pNtk);
    int num_parts = pNtk->pPdb->num_parts();
    if (num_parts <= 0)
    {
        part_id max_part = 0;
        Abc_Obj_t *pObj;
        int i;
        Abc_NtkForEachObj(pNtk, pObj, i)
            if (Abc_ObjGetPartId(pObj) != ABC_PART_ID_NONE)
                max_part = std::max(max_part, Abc_ObjGetPartId(pObj));
        num_parts = static_cast<int>(max_part) + 1;
    }
    int balance_pct = cfg.balance_pct >= 0 ? cfg.balance_pct : pNtk->pPdb->balance_pct();
    if (balance_pct < 0)
        balance_pct = 2;
    const int growth_budget = static_cast<int>(
        static_cast<long long>(metrics.nodes) * cfg.replicate_growth_pct / 100);
    return {
        num_parts,
        balance_pct,
        metrics.hop,
        (metrics.nodes * 102 + 99) / 100,
        growth_budget,
        (metrics.cut_nets * 150 + 99) / 100,
    };
}

void RestorePdbMetadata(Abc_Ntk_t *pNtk, const EntryLimits &limits)
{
    const Metrics metrics = ComputeMetrics(pNtk);
    Abc_NtkSetPartStats(pNtk, limits.num_parts, metrics.cut_nets, metrics.hop);
    pNtk->pPdb->set_balance_pct(limits.balance_pct);
}

} // namespace fox::csr::detail
```

- [ ] **Step 4: Build and run the new test**

Run:

```bash
rtk make release
rtk ./release/test_csr
```

Expected: build succeeds and `test_csr` exits 0.

- [ ] **Step 5: Commit**

```bash
rtk git add src/csr/csr_internal.hpp src/csr/csr_state.cpp src/csr/CMakeLists.txt src/test_csr.cpp src/CMakeLists.txt
rtk git commit -m "Add CSR optimization state tests"
```

## Task 2: Add shared limits, growth accounting, and deterministic search budgets

**Files:**
- Modify: `src/csr/csr_internal.hpp`
- Modify: `src/csr/csr_state.cpp`
- Modify: `src/test_csr.cpp`

- [ ] **Step 1: Write failing tests for non-refunding growth and deterministic budget exhaustion**

Append tests that require these APIs:

```cpp
bool TestGrowthBudgetDoesNotRefund()
{
    fox::csr::detail::GrowthTracker growth(5);
    bool ok = true;
    ok &= ExpectEqual("initial growth", growth.used(), 0);
    ok &= ExpectEqual("consume three", growth.TryConsume(3), 1);
    growth.RecordDeletion(2);
    ok &= ExpectEqual("deletion does not refund", growth.used(), 3);
    ok &= ExpectEqual("reject over budget", growth.TryConsume(3), 0);
    ok &= ExpectEqual("consume remainder", growth.TryConsume(2), 1);
    return ok;
}

bool TestSearchBudgetStopsAtExactLimit()
{
    fox::csr::detail::SearchBudget budget;
    bool ok = true;
    for (int i = 0; i < 128; ++i)
        ok &= ExpectEqual("sat call allowed", budget.TrySatCall(), 1);
    ok &= ExpectEqual("sat call 129 rejected", budget.TrySatCall(), 0);
    return ok;
}
```

Add both calls to `main()`.

- [ ] **Step 2: Run the test and verify it fails to compile**

```bash
rtk make release
```

Expected: missing `GrowthTracker` and `SearchBudget` errors.

- [ ] **Step 3: Implement the counters exactly**

Add to `csr_internal.hpp`:

```cpp
class GrowthTracker {
public:
    explicit GrowthTracker(int budget) : budget_(budget) {}
    bool TryConsume(int positive_net_growth);
    void RecordDeletion(int) {}
    int used() const { return used_; }
    int remaining() const { return budget_ - used_; }
private:
    int budget_ = 0;
    int used_ = 0;
};

struct SearchBudget {
    static constexpr int kMaxBeamStatesPerRound = 512;
    static constexpr int kMaxDivisorSetsPerNode = 32;
    static constexpr int kMaxSatCallsPerNode = 128;
    static constexpr int kMaxSuccessfulPlansPerNode = 4;
    static constexpr int kMaxClustersPerDriverPart = 16;

    int beam_states = 0;
    int divisor_sets = 0;
    int sat_calls = 0;
    int successful_plans = 0;
    int clusters = 0;

    bool TryBeamState();
    bool TryDivisorSet();
    bool TrySatCall();
    bool TrySuccessfulPlan();
    bool TryCluster();
};

struct OptimizationState {
    Abc_Ntk_t *pNtk = nullptr;
    EntryLimits limits;
    Metrics entry;
    Metrics current;
    GrowthTracker growth;
    int trajectory_id = 0;
    std::vector<int> part_sizes;

    OptimizationState(Abc_Ntk_t *pNetwork, const EntryLimits &entry_limits,
                      int tid);
    void AttachNetwork(Abc_Ntk_t *pNetwork);
    bool Audit();
};
```

Implement each method in `csr_state.cpp` using pre-increment checks. `TrySatCall()` must be:

```cpp
bool SearchBudget::TrySatCall()
{
    if (sat_calls >= kMaxSatCallsPerNode)
        return false;
    ++sat_calls;
    return true;
}
```

Implement `GrowthTracker::TryConsume`:

```cpp
bool GrowthTracker::TryConsume(int positive_net_growth)
{
    if (positive_net_growth <= 0)
        return true;
    if (used_ + positive_net_growth > budget_)
        return false;
    used_ += positive_net_growth;
    return true;
}
```

Implement `OptimizationState` in `csr_state.cpp`:

```cpp
OptimizationState::OptimizationState(Abc_Ntk_t *pNetwork,
                                     const EntryLimits &entry_limits,
                                     int tid)
    : pNtk(pNetwork), limits(entry_limits), entry(ComputeMetrics(pNetwork)),
      current(entry), growth(entry_limits.growth_budget), trajectory_id(tid)
{
    fox::cpr::partition_sizes(pNtk, limits.num_parts, part_sizes);
}

void OptimizationState::AttachNetwork(Abc_Ntk_t *pNetwork)
{
    pNtk = pNetwork;
    current = ComputeMetrics(pNtk);
    fox::cpr::partition_sizes(pNtk, limits.num_parts, part_sizes);
}

bool OptimizationState::Audit()
{
    current = ComputeMetrics(pNtk);
    return current.hop <= limits.hop_limit
        && current.nodes <= limits.node_limit
        && current.cut_nets <= limits.cutnet_limit
        && growth.used() <= limits.growth_budget;
}
```

- [ ] **Step 4: Run the focused test**

```bash
rtk make release
rtk ./release/test_csr
```

Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
rtk git add src/csr/csr_internal.hpp src/csr/csr_state.cpp src/test_csr.cpp
rtk git commit -m "Add CSR search and growth budgets"
```

## Task 3: Change CSR ownership to frame-based trajectory execution

**Files:**
- Modify: `src/csr/csr.hpp`
- Modify: `src/csr/csr_internal.hpp`
- Modify: `src/csr/csr_state.cpp`
- Modify: `src/csr/csr.cpp`
- Modify: `src/main.cpp`
- Modify: `src/test_csr.cpp`

- [ ] **Step 1: Write failing tests for trajectory result ordering and metadata restoration**

Add to `test_csr.cpp`:

```cpp
bool TestTrajectoryOrdering()
{
    using fox::csr::detail::TrajectoryResult;
    TrajectoryResult a{nullptr, {90, 12, 4, 100}, 1, true};
    TrajectoryResult b{nullptr, {90, 11, 4, 101}, 2, true};
    TrajectoryResult c{nullptr, {91, 1, 1, 1}, 0, true};
    bool ok = true;
    ok &= ExpectEqual("lower cutnet wins tie", fox::csr::detail::BetterResult(b, a), 1);
    ok &= ExpectEqual("cutedge remains primary", fox::csr::detail::BetterResult(c, a), 0);
    return ok;
}

bool TestDuplicatedTrajectoryUsesCapturedBalance()
{
    StateTestNtk base = CreateStateTestNtk();
    fox::csr::Config cfg;
    auto limits = fox::csr::detail::CaptureEntryLimits(base.pNtk, cfg);
    Abc_Ntk_t *pDup = Abc_NtkDup(base.pNtk);
    fox::csr::detail::OptimizationState state(pDup, limits, 0);
    bool ok = true;
    ok &= ExpectEqual("duplicate Pdb balance invalid", pDup->pPdb->balance_pct(), -1);
    ok &= ExpectEqual("state keeps entry balance", state.limits.balance_pct, 17);
    Abc_NtkDelete(pDup);
    Abc_NtkDelete(base.pNtk);
    return ok;
}
```

- [ ] **Step 2: Run the test and verify the missing trajectory types fail**

```bash
rtk make release
```

Expected: missing `TrajectoryResult` and `BetterResult`.

- [ ] **Step 3: Implement trajectory ownership primitives and change the public API**

Change `csr.hpp`:

```cpp
struct Config {
    // existing fields
    int num_trajectories = 1;
};

bool ApplyCsr(Abc_Frame_t *pAbc, const Config &cfg);
```

Add to `csr_internal.hpp`:

```cpp
struct TrajectoryResult {
    Abc_Ntk_t *pNtk = nullptr;
    Metrics metrics;
    int trajectory_id = 0;
    bool valid = false;
};

bool BetterResult(const TrajectoryResult &lhs, const TrajectoryResult &rhs);
```

Implement `BetterResult` using a tuple:

```cpp
return std::tie(lhs.metrics.cut_edges,
                lhs.metrics.cut_nets,
                lhs.metrics.hop,
                lhs.metrics.nodes,
                lhs.trajectory_id)
     < std::tie(rhs.metrics.cut_edges,
                rhs.metrics.cut_nets,
                rhs.metrics.hop,
                rhs.metrics.nodes,
                rhs.trajectory_id);
```

Refactor the old `ApplyCsr(Abc_Ntk_t *, ...)` body into a private trajectory function:

```cpp
static bool optimize_trajectory(Abc_Ntk_t *&pNtk, const Config &cfg,
                                detail::OptimizationState &state,
                                int trajectory_id);
```

Thread `OptimizationState &state` into Phase 0, Phase 1, Phase 2, and optional balance repair. Replace all post-entry reads of `pNtk->pPdb->num_parts()` or `balance_pct()` with `state.limits.num_parts` and `state.limits.balance_pct`.

Run `-b` balance repair transactionally at the end of each trajectory: duplicate the current trajectory into `pRepair`, run `cpr::enforce_balance` on `pRepair` with the frozen entry scalars, and recompute all metrics. If cut-edge does not increase and every hard constraint passes, delete the old trajectory network and replace its pointer with `pRepair`; otherwise delete `pRepair` and keep the old trajectory unchanged.

Implement the new frame wrapper so the original frame network remains untouched until a winner exists:

```cpp
bool ApplyCsr(Abc_Frame_t *pAbc, const Config &cfg)
{
    Abc_Ntk_t *pEntry = Abc_FrameReadNtk(pAbc);
    if (!pEntry) { printf("csr: network is null\n"); return false; }
    if (!Abc_NtkIsLogic(pEntry))
    { printf("csr: network must be logic (not AIG)\n"); return false; }
    if (!pEntry->pPdb)
    { printf("csr: no partition database (run hpart first)\n"); return false; }

    const detail::EntryLimits limits = detail::CaptureEntryLimits(pEntry, cfg);
    std::vector<detail::TrajectoryResult> results;
    for (int tid = 0; tid < cfg.num_trajectories; ++tid)
    {
        Abc_Ntk_t *pTrack = Abc_NtkDup(pEntry);
        if (!pTrack)
            continue;
        detail::OptimizationState state(pTrack, limits, tid);
        if (optimize_trajectory(pTrack, cfg, state, tid))
            results.push_back({pTrack, detail::ComputeMetrics(pTrack), tid, true});
        else
            Abc_NtkDelete(pTrack);
    }
    if (results.empty())
        return false;

    auto best = std::min_element(results.begin(), results.end(), detail::BetterResult);
    Abc_Ntk_t *pWinner = best->pNtk;
    detail::RestorePdbMetadata(pWinner, limits);
    for (auto &result : results)
        if (result.pNtk != pWinner)
            Abc_NtkDelete(result.pNtk);
    Abc_FrameReplaceCurrentNetwork(pAbc, pWinner);
    return true;
}
```

Adapt `main.cpp:838`:

```cpp
return fox::csr::ApplyCsr(pAbc, cfg) ? 0 : 1;
```

- [ ] **Step 4: Build and run existing tests plus a CLI smoke test**

```bash
rtk make release
rtk ./release/test_csr
rtk ./release/test_hop
rtk ./release/test_cpr
rtk ./release/FoxSYN -c "read regression/SimpleCircuits/mcnc/alu4.v; st; if -K 6; hpart -N 4 --load-part regression/parts_n4_flat/alu4.part; csr -T 1; ps"
```

Expected: all test executables exit 0; CLI prints a successful CSR summary and a valid PDB line.

- [ ] **Step 5: Commit**

```bash
rtk git add src/csr/csr.hpp src/csr/csr_internal.hpp src/csr/csr_state.cpp src/csr/csr.cpp src/main.cpp src/test_csr.cpp
rtk git commit -m "Make CSR trajectory execution transactional"
```

## Task 4: Add deterministic policies and the `-T` interface

**Files:**
- Modify: `src/csr/csr.hpp`
- Modify: `src/csr/csr_internal.hpp`
- Modify: `src/csr/csr_state.cpp`
- Modify: `src/csr/csr.cpp`
- Modify: `src/main.cpp`
- Modify: `src/test_csr.cpp`

- [ ] **Step 1: Write failing comparator tests for equal-weight candidates**

Add a test with equal weights but different IDs:

```cpp
bool TestCutCandidateTotalOrder()
{
    std::vector<fox::csr::detail::CutCandidate> v = {
        {9, 1, 5, 2},
        {3, 2, 5, 1},
        {3, 0, 5, 1},
    };
    std::sort(v.begin(), v.end(), fox::csr::detail::CutCandidateLess{});
    bool ok = true;
    ok &= ExpectEqual("first node id", v[0].node_id, 3);
    ok &= ExpectEqual("first fanin", v[0].iFanin, 0);
    ok &= ExpectEqual("last node id", v[2].node_id, 9);
    return ok;
}
```

- [ ] **Step 2: Verify the comparator test fails**

```bash
rtk make release
```

Expected: internal candidate type is unavailable or lacks a total comparator.

- [ ] **Step 3: Move the candidate type to the internal header and implement policy keys**

Define:

```cpp
enum class TrajectoryPolicy {
    GainFirst = 0,
    BoundaryConcentration = 1,
    ScarcityFirst = 2,
};

struct CutCandidate {
    int node_id = -1;
    int iFanin = -1;
    int weight = 0;
    int target_options = 0;
};

struct CutCandidateLess {
    bool operator()(const CutCandidate &a, const CutCandidate &b) const;
};
```

The `GainFirst` comparator must use the total key:

```cpp
return std::tuple{-a.weight, a.node_id, a.iFanin}
     < std::tuple{-b.weight, b.node_id, b.iFanin};
```

Add `-T` parsing to `Csr_Command`:

```cpp
case 'T':
    if (i + 1 >= argc) { printf("csr: -T requires a number\n"); return 1; }
    cfg.num_trajectories = std::atoi(argv[++i]);
    if (cfg.num_trajectories < 1 || cfg.num_trajectories > 3)
    { printf("csr: -T must be 1-3\n"); return 1; }
    break;
```

Update usage to include `[-T num]`, default 1.

- [ ] **Step 4: Verify deterministic output on a frozen partition**

Run the same command three times and compare the two CSR summary lines:

```bash
rtk ./release/FoxSYN -c "read regression/SimpleCircuits/mcnc/alu4.v; st; if -K 6; hpart -N 4 --load-part regression/parts_n4_flat/alu4.part; csr -T 1"
rtk ./release/FoxSYN -c "read regression/SimpleCircuits/mcnc/alu4.v; st; if -K 6; hpart -N 4 --load-part regression/parts_n4_flat/alu4.part; csr -T 1"
rtk ./release/FoxSYN -c "read regression/SimpleCircuits/mcnc/alu4.v; st; if -K 6; hpart -N 4 --load-part regression/parts_n4_flat/alu4.part; csr -T 1"
```

Expected: the final cut-edge line and phase counts are byte-identical in all three runs.

- [ ] **Step 5: Commit**

```bash
rtk git add src/csr/csr.hpp src/csr/csr_internal.hpp src/csr/csr_state.cpp src/csr/csr.cpp src/main.cpp src/test_csr.cpp
rtk git commit -m "Make CSR candidate ordering deterministic"
```

## Task 5: Implement bounded Phase 0 compound relocation

**Files:**
- Modify: `src/csr/csr_internal.hpp`
- Modify: `src/csr/csr.cpp`
- Modify: `src/test_csr.cpp`

- [ ] **Step 1: Add a failing two-step relocation test**

Add this exact fixture to `test_csr.cpp`. Its path is `PI(P0) -> A(P1) -> B(P1) -> Sink(P0)`. Moving only A or only B keeps two cut-edges; moving A and B together reduces two cut-edges to zero.

```cpp
struct CompoundMoveNtk {
    Abc_Ntk_t *pNtk = nullptr;
    Abc_Obj_t *pPi = nullptr;
    Abc_Obj_t *pA = nullptr;
    Abc_Obj_t *pB = nullptr;
    Abc_Obj_t *pSink = nullptr;
    Abc_Obj_t *pPo = nullptr;
};

CompoundMoveNtk CreateCompoundMoveNtk()
{
    CompoundMoveNtk t;
    t.pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    t.pPi = Abc_NtkCreatePi(t.pNtk);
    t.pA = Abc_NtkCreateNode(t.pNtk);
    t.pB = Abc_NtkCreateNode(t.pNtk);
    t.pSink = Abc_NtkCreateNode(t.pNtk);
    t.pPo = Abc_NtkCreatePo(t.pNtk);
    Abc_ObjAddFanin(t.pA, t.pPi);
    Abc_ObjAddFanin(t.pB, t.pA);
    Abc_ObjAddFanin(t.pSink, t.pB);
    Abc_ObjAddFanin(t.pPo, t.pSink);
    SetNodeFunction(t.pA);
    SetNodeFunction(t.pB);
    SetNodeFunction(t.pSink);
    Abc_ObjSetPartId(t.pPi, 0);
    Abc_ObjSetPartId(t.pA, 1);
    Abc_ObjSetPartId(t.pB, 1);
    Abc_ObjSetPartId(t.pSink, 0);
    Abc_NtkSetPartStats(t.pNtk, 2, Abc_NtkComputeCutSize(t.pNtk),
                        Abc_NtkComputeHopNum(t.pNtk));
    t.pNtk->pPdb->set_balance_pct(99);
    return t;
}
```

Assert:

```cpp
auto sequence = fox::csr::detail::FindBestRelocationSequence(
    test.pNtk, state, fox::csr::detail::TrajectoryPolicy::GainFirst);
ok &= ExpectEqual("compound move length", static_cast<int>(sequence.steps.size()), 2);
ok &= ExpectEqual("compound delta", sequence.cutedge_delta, -2);
ok &= ExpectEqual("apply sequence", fox::csr::detail::ApplyRelocationSequence(
    test.pNtk, state, sequence), 1);
ok &= ExpectEqual("hop preserved", Abc_NtkComputeHopNum(test.pNtk), entry_hop);
```

Initialize the test state with `Config cfg; cfg.balance_pct = 99;`, `CaptureEntryLimits`, and `OptimizationState`, then delete the network after the assertions.

- [ ] **Step 2: Run the test and verify it fails**

```bash
rtk make release
rtk ./release/test_csr
```

Expected: missing relocation sequence APIs.

- [ ] **Step 3: Implement beam search with exact transaction checks**

Add internal types:

```cpp
struct RelocationStep { int node_id; part_id from; part_id to; };
struct RelocationSequence {
    std::vector<RelocationStep> steps;
    int cutedge_delta = 0;
};

RelocationSequence FindBestRelocationSequence(
    Abc_Ntk_t *pNtk, OptimizationState &state, TrajectoryPolicy policy);
bool ApplyRelocationSequence(Abc_Ntk_t *pNtk, OptimizationState &state,
                             const RelocationSequence &sequence);
```

Implement the search with these exact bounds:

```cpp
constexpr int kSeedLimit = 64;
constexpr int kBeamWidth = 8;
constexpr int kMaxDepth = 3;
```

For each expanded state:

1. Apply its part-ID log.
2. Reject immediately if any target partition exceeds `max_allowed`.
3. Reject if `Abc_NtkComputeHopNum(pNtk) > state.limits.hop_limit`.
4. Reject if `Abc_NtkComputeCutSize(pNtk) > state.limits.cutnet_limit`.
5. Rank survivors by `(cutedge_delta, sequence_length, node IDs, target part IDs)`.
6. Roll back the part-ID log before expanding the next state.

Retrofit the existing single move and swap paths to perform the same exact `Abc_NtkComputeCutSize` check before commit. Call compound relocation only after existing move and swap loops stall. Accept only a final negative cut-edge delta.

- [ ] **Step 4: Run focused and smoke tests**

```bash
rtk make release
rtk ./release/test_csr
rtk ./release/FoxSYN -c "read regression/SimpleCircuits/mcnc/alu4.v; st; if -K 6; hpart -N 4 --load-part regression/parts_n4_flat/alu4.part; csr -T 1 -v"
```

Expected: unit test passes; verbose output reports `phase0 compound=<count>`; hop and cut-net remain within limits.

- [ ] **Step 5: Commit**

```bash
rtk git add src/csr/csr_internal.hpp src/csr/csr.cpp src/test_csr.cpp
rtk git commit -m "Add CSR compound relocation search"
```

## Task 6: Implement exact incremental `HopState`

**Files:**
- Create: `src/csr/csr_hop.cpp`
- Modify: `src/csr/csr_internal.hpp`
- Modify: `src/csr/CMakeLists.txt`
- Modify: `src/test_csr.cpp`

- [ ] **Step 1: Write failing increase, decrease, and rollback tests**

Add this exact three-level DAG fixture. `low` has arrival 0 in P0; `cross` has arrival 1 in P1; `high` has arrival 2 in P2; `sink` starts in P0 driven by `low`.

```cpp
struct HopPropagationNtk {
    Abc_Ntk_t *pNtk = nullptr;
    Abc_Obj_t *pPi = nullptr;
    Abc_Obj_t *pLow = nullptr;
    Abc_Obj_t *pCross = nullptr;
    Abc_Obj_t *pHigh = nullptr;
    Abc_Obj_t *pSink = nullptr;
    Abc_Obj_t *pPoHigh = nullptr;
    Abc_Obj_t *pPoSink = nullptr;
};

HopPropagationNtk CreateHopPropagationNtk()
{
    HopPropagationNtk t;
    t.pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    t.pPi = Abc_NtkCreatePi(t.pNtk);
    t.pLow = Abc_NtkCreateNode(t.pNtk);
    t.pCross = Abc_NtkCreateNode(t.pNtk);
    t.pHigh = Abc_NtkCreateNode(t.pNtk);
    t.pSink = Abc_NtkCreateNode(t.pNtk);
    t.pPoHigh = Abc_NtkCreatePo(t.pNtk);
    t.pPoSink = Abc_NtkCreatePo(t.pNtk);
    Abc_ObjAddFanin(t.pLow, t.pPi);
    Abc_ObjAddFanin(t.pCross, t.pPi);
    Abc_ObjAddFanin(t.pHigh, t.pCross);
    Abc_ObjAddFanin(t.pSink, t.pLow);
    Abc_ObjAddFanin(t.pPoHigh, t.pHigh);
    Abc_ObjAddFanin(t.pPoSink, t.pSink);
    SetNodeFunction(t.pLow);
    SetNodeFunction(t.pCross);
    SetNodeFunction(t.pHigh);
    SetNodeFunction(t.pSink);
    Abc_ObjSetPartId(t.pPi, 0);
    Abc_ObjSetPartId(t.pLow, 0);
    Abc_ObjSetPartId(t.pCross, 1);
    Abc_ObjSetPartId(t.pHigh, 2);
    Abc_ObjSetPartId(t.pSink, 0);
    return t;
}
```

Patch `sink` from `low` to `high` and require:

```cpp
fox::csr::detail::HopState hop;
ok &= ExpectEqual("hop init", hop.Initialize(test.pNtk), 1);
auto txn = hop.BeginTransaction();
Abc_ObjPatchFanin(test.pSink, test.pLow, test.pHigh);
ok &= ExpectEqual("propagate detects limit", hop.PropagateFrom(
    test.pNtk, {test.pSink->Id}, entry_hop, txn), 0);
hop.Rollback(txn);
Abc_ObjPatchFanin(test.pSink, test.pHigh, test.pLow);
ok &= ExpectEqual("rollback matches full", hop.VerifyAgainstFull(test.pNtk), 1);
```

For the decrease test, extend the fixture before initialization with:

```cpp
Abc_Obj_t *pTail1 = Abc_NtkCreateNode(t.pNtk);
Abc_Obj_t *pTail2 = Abc_NtkCreateNode(t.pNtk);
Abc_ObjAddFanin(pTail1, t.pSink);
Abc_ObjAddFanin(pTail2, pTail1);
SetNodeFunction(pTail1);
SetNodeFunction(pTail2);
Abc_ObjSetPartId(pTail1, 0);
Abc_ObjSetPartId(pTail2, 0);
Abc_ObjPatchFanin(t.pSink, t.pLow, t.pHigh);
```

Initialize at hop 3, patch `sink` back to `low`, propagate from `sink`, and assert `arrival(sink)`, `arrival(tail1)`, and `arrival(tail2)` are all 0 and `VerifyAgainstFull` succeeds.

- [ ] **Step 2: Verify the tests fail**

```bash
rtk make release
```

Expected: missing `HopState`.

- [ ] **Step 3: Implement topology-ordered propagation**

Declare in `csr_internal.hpp`:

```cpp
class HopState {
public:
    struct Change { int obj_id; int old_arrival; };
    struct Transaction {
        std::vector<Change> changes;
        std::vector<char> logged;
    };

    bool Initialize(Abc_Ntk_t *pNtk);
    Transaction BeginTransaction() const;
    bool PropagateFrom(Abc_Ntk_t *pNtk, const std::vector<int> &start_ids,
                       int hop_limit, Transaction &txn);
    void Rollback(Transaction &txn);
    bool VerifyAgainstFull(Abc_Ntk_t *pNtk) const;
    int arrival(int obj_id) const;
    int topo_rank(int obj_id) const;
private:
    std::vector<int> arrival_;
    std::vector<int> topo_rank_;
};
```

In `csr_hop.cpp`, build ranks from `Abc_NtkDfs`, use a min-priority queue keyed by `(topo_rank, obj_id)`, and recompute each popped node over all part-stat fanins. Log each old arrival once. Enqueue all part-stat fanouts only when the value changes. Reject on any value above `hop_limit`.

`VerifyAgainstFull` must independently recompute the full arrival vector, not only compare the maximum hop.

- [ ] **Step 4: Build and run tests**

```bash
rtk make release
rtk ./release/test_csr
```

Expected: increase is rejected and rolled back; decrease propagates; full-vector verification succeeds.

- [ ] **Step 5: Commit**

```bash
rtk git add src/csr/csr_hop.cpp src/csr/csr_internal.hpp src/csr/CMakeLists.txt src/test_csr.cpp
rtk git commit -m "Add exact incremental CSR hop state"
```

## Task 7: Aggregate Phase 2 candidates by driver and target partition

**Files:**
- Modify: `src/csr/csr_internal.hpp`
- Modify: `src/csr/csr.cpp`
- Modify: `src/test_csr.cpp`

- [ ] **Step 1: Write a failing aggregation and score test**

Create the exact graph below: two PIs in P0/P2 feed driver D in P0; D feeds three consumers in target P1.

```cpp
Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
Abc_Obj_t *pA = Abc_NtkCreatePi(pNtk);
Abc_Obj_t *pB = Abc_NtkCreatePi(pNtk);
Abc_Obj_t *pD = Abc_NtkCreateNode(pNtk);
Abc_ObjAddFanin(pD, pA);
Abc_ObjAddFanin(pD, pB);
SetNodeFunction(pD);
Abc_ObjSetPartId(pA, 0);
Abc_ObjSetPartId(pB, 2);
Abc_ObjSetPartId(pD, 0);
for (int i = 0; i < 3; ++i)
{
    Abc_Obj_t *pConsumer = Abc_NtkCreateNode(pNtk);
    Abc_ObjAddFanin(pConsumer, pD);
    SetNodeFunction(pConsumer);
    Abc_ObjSetPartId(pConsumer, 1);
    Abc_Obj_t *pPo = Abc_NtkCreatePo(pNtk);
    Abc_ObjAddFanin(pPo, pConsumer);
}
```

Require one candidate with saved edges 3, added boundary edges 2, and net gain 1:

```cpp
auto candidates = fox::csr::detail::CollectReplicationCandidates(pNtk);
ok &= ExpectEqual("one driver-target candidate", candidates.size(), 1);
ok &= ExpectEqual("saved outgoing edges", candidates[0].saved_edges, 3);
ok &= ExpectEqual("added boundary edges", candidates[0].added_edges, 2);
ok &= ExpectEqual("net gain", candidates[0].net_gain(), 1);
Abc_NtkDelete(pNtk);
```

- [ ] **Step 2: Verify the test fails**

```bash
rtk make release
```

Expected: missing replication candidate API.

- [ ] **Step 3: Implement aggregation and deterministic scoring**

Add:

```cpp
struct ReplicationKey { int driver_id; part_id target_part; };
struct ReplicationCandidate {
    ReplicationKey key;
    int saved_edges = 0;
    int added_edges = 0;
    int cutnet_delta = 0;
    int node_cost = 1;
    int net_gain() const { return saved_edges - added_edges; }
};

std::vector<ReplicationCandidate> CollectReplicationCandidates(Abc_Ntk_t *pNtk);
```

Collect candidates once per `(driver_id, target_part)`, count every target-part fanout as a saved cut-edge, and count driver fanins outside the target as added boundary edges. Sort by:

```cpp
(-net_gain / node_cost, -net_gain, cutnet_delta, driver_id, target_part)
```

Use cross multiplication instead of floating-point division for the ratio comparison.

Replace Phase 2's edge-by-edge loop with the aggregated list while preserving single-node replication behavior.

- [ ] **Step 4: Verify Phase 2 behavior**

```bash
rtk make release
rtk ./release/test_csr
rtk ./release/FoxSYN -c "read regression/SimpleCircuits/mcnc/alu4.v; st; if -K 6; hpart -N 4 --load-part regression/parts_n4_flat/alu4.part; csr -G 2 -T 1 -v"
```

Expected: unit test passes; every accepted replication strictly reduces cut-edge and respects cut-net.

- [ ] **Step 5: Commit**

```bash
rtk git add src/csr/csr_internal.hpp src/csr/csr.cpp src/test_csr.cpp
rtk git commit -m "Aggregate CSR replication candidates"
```

## Task 8: Add bounded replication clusters with transactional hop rollback

**Files:**
- Modify: `src/csr/csr_internal.hpp`
- Modify: `src/csr/csr.cpp`
- Modify: `src/test_csr.cpp`

- [ ] **Step 1: Write a failing cluster-only gain test**

Use this exact graph. `F(P0)` is driven by `Z(P1)`. `D(P0)` is driven by `F(P0)` and `X(P2)`. D has two consumers in target P1. D-only duplication saves two edges and adds two; duplicating F with D leaves only X as an external boundary and produces gain 1.

```cpp
struct ReplicationClusterNtk {
    Abc_Ntk_t *pNtk = nullptr;
    Abc_Obj_t *pD = nullptr;
};

ReplicationClusterNtk CreateReplicationClusterNtk()
{
    ReplicationClusterNtk t;
    t.pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *pZ = Abc_NtkCreatePi(t.pNtk);
    Abc_Obj_t *pX = Abc_NtkCreatePi(t.pNtk);
    Abc_Obj_t *pF = Abc_NtkCreateNode(t.pNtk);
    t.pD = Abc_NtkCreateNode(t.pNtk);
    Abc_ObjAddFanin(pF, pZ);
    Abc_ObjAddFanin(t.pD, pF);
    Abc_ObjAddFanin(t.pD, pX);
    SetNodeFunction(pF);
    SetNodeFunction(t.pD);
    Abc_ObjSetPartId(pZ, 1);
    Abc_ObjSetPartId(pX, 2);
    Abc_ObjSetPartId(pF, 0);
    Abc_ObjSetPartId(t.pD, 0);
    for (int i = 0; i < 2; ++i)
    {
        Abc_Obj_t *pConsumer = Abc_NtkCreateNode(t.pNtk);
        Abc_ObjAddFanin(pConsumer, t.pD);
        SetNodeFunction(pConsumer);
        Abc_ObjSetPartId(pConsumer, 1);
        Abc_Obj_t *pPo = Abc_NtkCreatePo(t.pNtk);
        Abc_ObjAddFanin(pPo, pConsumer);
    }
    return t;
}
```

Require a two-node cluster with positive gain:

```cpp
auto test = CreateReplicationClusterNtk();
fox::csr::Config cfg;
auto limits = fox::csr::detail::CaptureEntryLimits(test.pNtk, cfg);
limits.growth_budget = 2;
limits.node_limit = Abc_NtkNodeNum(test.pNtk) + 2;
fox::csr::detail::OptimizationState state(test.pNtk, limits, 0);
fox::csr::detail::HopState hop;
hop.Initialize(test.pNtk);
auto candidate = fox::csr::detail::CollectReplicationCandidates(test.pNtk).front();
auto cluster = fox::csr::detail::FindBestReplicationCluster(
    test.pNtk, state, candidate, hop);
ok &= ExpectEqual("cluster size", cluster.node_ids.size(), 2);
ok &= ExpectEqual("cluster positive gain", cluster.cutedge_delta < 0, 1);
ok &= ExpectEqual("apply cluster", fox::csr::detail::TryReplicationCluster(
    test.pNtk, state, hop, cluster), 1);
ok &= ExpectEqual("hop exact", hop.VerifyAgainstFull(test.pNtk), 1);

auto blocked = CreateReplicationClusterNtk();
auto blocked_limits = fox::csr::detail::CaptureEntryLimits(blocked.pNtk, cfg);
blocked_limits.growth_budget = 2;
blocked_limits.node_limit = Abc_NtkNodeNum(blocked.pNtk) + 2;
fox::csr::detail::OptimizationState blocked_state(blocked.pNtk,
                                                  blocked_limits, 0);
fox::csr::detail::HopState blocked_hop;
blocked_hop.Initialize(blocked.pNtk);
auto blocked_candidate =
    fox::csr::detail::CollectReplicationCandidates(blocked.pNtk).front();
auto blocked_cluster = fox::csr::detail::FindBestReplicationCluster(
    blocked.pNtk, blocked_state, blocked_candidate, blocked_hop);
ok &= ExpectEqual("consume all shared growth",
                  blocked_state.growth.TryConsume(blocked_limits.growth_budget), 1);
const int nodes_before = Abc_NtkNodeNum(blocked.pNtk);
ok &= ExpectEqual("phase2 rejected after phase1 budget exhaustion",
                  fox::csr::detail::TryReplicationCluster(
                      blocked.pNtk, blocked_state, blocked_hop, blocked_cluster), 0);
ok &= ExpectEqual("rejected cluster leaves node count",
                  Abc_NtkNodeNum(blocked.pNtk), nodes_before);
Abc_NtkDelete(test.pNtk);
Abc_NtkDelete(blocked.pNtk);
```

- [ ] **Step 2: Verify the test fails**

```bash
rtk make release
```

Expected: missing cluster APIs.

- [ ] **Step 3: Implement cluster enumeration and transaction logs**

Use these hard bounds:

```cpp
constexpr int kMaxClusterDepth = 2;
constexpr int kMaxClusterNodes = 3;
```

Add the exact internal API:

```cpp
struct ReplicationCluster {
    ReplicationKey key;
    std::vector<int> node_ids;
    int cutedge_delta = 0;
    int cutnet_delta = 0;
    int positive_net_growth = 0;
};

ReplicationCluster FindBestReplicationCluster(
    Abc_Ntk_t *pNtk, OptimizationState &state,
    const ReplicationCandidate &candidate, HopState &hop);
bool TryReplicationCluster(Abc_Ntk_t *pNtk, OptimizationState &state,
                           HopState &hop, const ReplicationCluster &cluster);
```

Enumerate only fanin logic nodes whose duplication removes at least one cluster boundary edge. Build duplicates in original topological order. Internal duplicate-to-duplicate edges are same-partition and must not count toward cut-edge or cut-net.

For each tentative cluster:

1. Check `GrowthTracker::TryConsume` only after all other checks succeed.
2. Patch target fanouts and retain an inverse patch log.
3. Run `Abc_NtkComputeCutSize` and reject above the entry limit.
4. Run `HopState::PropagateFrom`; on failure, restore arrival and topology.
5. Recompute cut-edge and accept only a strict decrease.
6. On rejection, delete duplicates in reverse topological order.

After each Phase 2 round, require `hop.VerifyAgainstFull(pNtk)`.

- [ ] **Step 4: Run tests and an ASan smoke case**

```bash
rtk make release
rtk ./release/test_csr
rtk make asan
rtk ./asan/FoxSYN -c "read regression/SimpleCircuits/mcnc/alu4.v; st; if -K 6; hpart -N 4 --load-part regression/parts_n4_flat/alu4.part; csr -T 1 -G 2"
```

Expected: no sanitizer diagnostics; cluster test passes; hop and cut-net remain legal.

- [ ] **Step 5: Commit**

```bash
rtk git add src/csr/csr_internal.hpp src/csr/csr.cpp src/test_csr.cpp
rtk git commit -m "Add CSR replication cluster search"
```

## Task 9: Add Phase 1 divisor metadata and exact hypothetical cut-net evaluation

**Files:**
- Modify: `src/csr/csr_internal.hpp`
- Modify: `src/csr/csr.cpp`
- Modify: `src/test_csr.cpp`

- [ ] **Step 1: Write failing ranking and cut-net tests**

Create a node with two old fanin drivers and two candidate divisors. Assert that a divisor with better counterexample coverage and lower cut-net delta ranks first, and that removing the last cross-partition fanout clears a cut-net:

```cpp
fox::csr::detail::DivisorInfo a{3, 1, 4, -1, 0, 3};
fox::csr::detail::DivisorInfo b{4, 1, 3, 0, 1, 4};
std::vector infos{b, a};
std::sort(infos.begin(), infos.end(), fox::csr::detail::DivisorInfoLess{});
ok &= ExpectEqual("coverage first", infos[0].obj_id, 3);

const int delta = fox::csr::detail::ComputeHypotheticalCutNetDelta(
    test.pNode, old_fanins, new_fanins);
ok &= ExpectEqual("last crossing fanout clears net", delta, -1);
```

- [ ] **Step 2: Verify the tests fail**

```bash
rtk make release
```

Expected: missing divisor metadata and hypothetical cut-net helper.

- [ ] **Step 3: Implement exact affected-net accounting**

Define:

```cpp
struct DivisorInfo {
    int obj_id;
    part_id part;
    int cex_coverage;
    int predicted_cutedge_delta;
    int predicted_cutnet_delta;
    int hop_arrival;
};

struct DivisorInfoLess {
    bool operator()(const DivisorInfo &a, const DivisorInfo &b) const;
};

int ComputeHypotheticalCutNetDelta(
    Abc_Obj_t *pConsumer,
    const std::vector<Abc_Obj_t *> &old_fanins,
    const std::vector<Abc_Obj_t *> &new_fanins);
```

`ComputeHypotheticalCutNetDelta` must consider the union of old and new fanin drivers. For each affected driver, determine whether it has any cross-partition fanout before and after replacing this consumer's fanin set. Return the difference in the number of affected cut-nets. Do not use cached `fCutNet` as the only source of truth.

Sort divisors by:

```cpp
(-cex_coverage,
 predicted_cutedge_delta,
 predicted_cutnet_delta,
 hop_arrival,
 obj_id)
```

Keep the best 64 metadata entries, but allow at most 32 actual divisor-set SAT evaluations per node.

- [ ] **Step 4: Run focused tests**

```bash
rtk make release
rtk ./release/test_csr
```

Expected: ranking and exact cut-net tests pass.

- [ ] **Step 5: Commit**

```bash
rtk git add src/csr/csr_internal.hpp src/csr/csr.cpp src/test_csr.cpp
rtk git commit -m "Rank CSR divisors by partition benefit"
```

## Task 10: Implement node-level multi-plan Phase 1 resub

**Files:**
- Modify: `src/csr/csr_internal.hpp`
- Modify: `src/csr/csr.cpp`
- Modify: `src/test_csr.cpp`

- [ ] **Step 1: Write failing plan-selection tests**

Test pure removal, joint removal, one-divisor replacement, two-divisor replacement, and the external-divisor rule independently of MFS by feeding legal `ResubPlan` objects to the selector:

```cpp
std::vector<fox::csr::detail::ResubPlan> plans = {
    {.cutedge_delta = -1, .cutnet_delta = 0, .predicted_hop = 3,
     .new_fanin_count = 2, .divisor_ids = {8}},
    {.cutedge_delta = -2, .cutnet_delta = 1, .predicted_hop = 3,
     .new_fanin_count = 1, .divisor_ids = {9}},
};
auto best = fox::csr::detail::SelectBestResubPlan(plans);
ok &= ExpectEqual("largest cutedge gain wins", best->divisor_ids[0], 9);
ok &= ExpectEqual("external divisor reduces two crossings",
                  fox::csr::detail::ExternalDivisorPlanAllowed(2, 1), 1);
ok &= ExpectEqual("one for one external replacement rejected",
                  fox::csr::detail::ExternalDivisorPlanAllowed(1, 1), 0);
```

Add an integration fixture where `x(P0)` and `y(P0)` feed both `d = x & y (P0)` and `consumer = x & y (P2)`. The consumer initially has two crossing fanins; replacing both with external-part divisor `d` leaves one crossing and preserves hop.

```cpp
struct JointResubNtk {
    Abc_Ntk_t *pNtk = nullptr;
    Abc_Obj_t *pDivisor = nullptr;
    Abc_Obj_t *pConsumer = nullptr;
};

JointResubNtk CreateJointResubNtk()
{
    JointResubNtk t;
    t.pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *pX = Abc_NtkCreatePi(t.pNtk);
    Abc_Obj_t *pY = Abc_NtkCreatePi(t.pNtk);
    t.pDivisor = Abc_NtkCreateNode(t.pNtk);
    t.pConsumer = Abc_NtkCreateNode(t.pNtk);
    Abc_Obj_t *pPo = Abc_NtkCreatePo(t.pNtk);
    Abc_Obj_t *pDivisorPo = Abc_NtkCreatePo(t.pNtk);
    Abc_ObjAddFanin(t.pDivisor, pX);
    Abc_ObjAddFanin(t.pDivisor, pY);
    Abc_ObjAddFanin(t.pConsumer, pX);
    Abc_ObjAddFanin(t.pConsumer, pY);
    Abc_ObjAddFanin(pPo, t.pConsumer);
    Abc_ObjAddFanin(pDivisorPo, t.pDivisor);
    SetNodeFunction(t.pDivisor);
    SetNodeFunction(t.pConsumer);
    Abc_ObjSetPartId(pX, 0);
    Abc_ObjSetPartId(pY, 0);
    Abc_ObjSetPartId(t.pDivisor, 0);
    Abc_ObjSetPartId(t.pConsumer, 2);
    Abc_NtkSetPartStats(t.pNtk, 3, Abc_NtkComputeCutSize(t.pNtk),
                        Abc_NtkComputeHopNum(t.pNtk));
    t.pNtk->pPdb->set_balance_pct(99);
    return t;
}
```

Run the Phase 1 internal entry and assert:

```cpp
auto t = CreateJointResubNtk();
fox::csr::Config cfg;
cfg.do_relocate = false;
cfg.replicate_growth_pct = 0;
auto limits = fox::csr::detail::CaptureEntryLimits(t.pNtk, cfg);
fox::csr::detail::OptimizationState state(t.pNtk, limits, 0);
fox::csr::detail::Phase1Stats stats;
ok &= ExpectEqual("joint phase1 succeeds",
                  fox::csr::detail::RunPhase1Resub(t.pNtk, state, cfg, stats), 1);
ok &= ExpectEqual("joint replacement counted", stats.joint_replacements, 1);
ok &= ExpectEqual("consumer now has one fanin",
                  Abc_ObjFaninNum(t.pConsumer), 1);
ok &= ExpectEqual("external divisor installed",
                  Abc_ObjFanin(t.pConsumer, 0)->Id, t.pDivisor->Id);
ok &= ExpectEqual("joint resub cutedge",
                  fox::csr::ComputeCutEdgeCount(t.pNtk), 1);
ok &= ExpectEqual("joint resub hop", Abc_NtkComputeHopNum(t.pNtk), 1);
Abc_NtkDelete(t.pNtk);
```

- [ ] **Step 2: Verify the tests fail**

```bash
rtk make release
```

Expected: missing resub plan APIs.

- [ ] **Step 3: Refactor Phase 1 around consumer-level targets**

Define:

```cpp
struct ResubPlan {
    std::vector<int> removed_fanin_indices;
    std::vector<int> divisor_ids;
    Hop_Obj_t *pFunc = nullptr;
    int cutedge_delta = 0;
    int cutnet_delta = 0;
    int predicted_hop = 0;
    int new_fanin_count = 0;
    int positive_net_growth = 0;
};

std::optional<ResubPlan> SelectBestResubPlan(
    const std::vector<ResubPlan> &plans);
bool ExternalDivisorPlanAllowed(int removed_crossings, int added_crossings);
bool ResubPlanAllowed(const ResubPlan &plan, const OptimizationState &state);

struct Phase1Stats {
    int attempts = 0;
    int successes = 0;
    int single_removals = 0;
    int joint_removals = 0;
    int joint_replacements = 0;
    int multi_divisor = 0;
};

bool RunPhase1Resub(Abc_Ntk_t *pNtk, OptimizationState &state,
                    const Config &cfg, Phase1Stats &stats);
```

For each consumer node:

1. Collect all crossing fanin indices.
2. Generate single-removal and pair-removal target sets in deterministic index order.
3. Generate zero-, one-, and two-divisor plans using ranked metadata.
4. Permit an external-part divisor only when new crossing fanins are strictly fewer than removed crossing fanins.
5. Count every `Abc_NtkMfsTryResubOnce` call with `SearchBudget::TrySatCall`.
6. Retain at most four SAT-successful plans.
7. Reject any plan whose predicted hop exceeds `hop_limit`, cut-net exceeds `cutnet_limit`, or growth exceeds the shared budget.
8. Select by `(cutedge_delta, cutnet_delta, predicted_hop, new_fanin_count, divisor IDs)`.
9. Commit at most one plan per node per round.

Implement `ResubPlanAllowed` as:

```cpp
bool ResubPlanAllowed(const ResubPlan &plan, const OptimizationState &state)
{
    return plan.cutedge_delta < 0
        && state.current.cut_nets + plan.cutnet_delta <= state.limits.cutnet_limit
        && plan.predicted_hop <= state.limits.hop_limit
        && plan.positive_net_growth <= state.growth.remaining();
}
```

Add a sequential-cap test: set `state.current.cut_nets = state.limits.cutnet_limit - 1`, accept one plan with `cutnet_delta = 1`, update `state.current.cut_nets`, and assert a second otherwise-identical plan with `cutnet_delta = 1` is rejected.

Before a round rollback, call `Mfs_ManStop`, stop reverse levels, destroy every vector containing network object pointers, then replace the trajectory network pointer with the snapshot.

- [ ] **Step 4: Run unit tests and frozen-partition CEC cases**

```bash
rtk make release
rtk ./release/test_csr
rtk ../release/FoxSYN -c "read SimpleCircuits/mcnc/alu4.v; st; if -K 6; hpart -N 4 --load-part parts_n4_flat/alu4.part; write /tmp/csr_before.blif; csr -T 1; write /tmp/csr_after.blif; cec /tmp/csr_before.blif /tmp/csr_after.blif" 
```

Run the CLI command from `regression/`.

Expected: `Networks are equivalent`; Phase 1 reports per-plan success counts; hard constraints remain legal.

- [ ] **Step 5: Commit**

```bash
rtk git add src/csr/csr_internal.hpp src/csr/csr.cpp src/test_csr.cpp
rtk git commit -m "Search multiple CSR resub plans"
```

## Task 11: Complete multi-trajectory execution and reporting

**Files:**
- Modify: `src/csr/csr_internal.hpp`
- Modify: `src/csr/csr.cpp`
- Modify: `src/main.cpp`
- Modify: `src/test_csr.cpp`

- [ ] **Step 1: Write failing policy-selection and loser-cleanup tests**

Add this internal helper signature so ownership can be tested without running all phases:

```cpp
using NetworkDeleteFn = void (*)(Abc_Ntk_t *);

TrajectoryResult TakeBestTrajectory(std::vector<TrajectoryResult> &results,
                                    const EntryLimits &limits,
                                    NetworkDeleteFn delete_fn);
```

In the test, create one `StateTestNtk`, duplicate it three times, and assign synthetic metrics:

```cpp
auto counting_delete = +[](Abc_Ntk_t *pNtk) {
    ++g_deleted_count;
    Abc_NtkDelete(pNtk);
};

std::vector<fox::csr::detail::TrajectoryResult> results = {
    {Abc_NtkDup(base.pNtk), {90, 12, 4, 100}, 0, true},
    {Abc_NtkDup(base.pNtk), {90, 11, 4, 101}, 1, true},
    {Abc_NtkDup(base.pNtk), {91, 1, 1, 1}, 2, true},
};
g_deleted_count = 0;
auto result = fox::csr::detail::TakeBestTrajectory(results, limits,
                                                   counting_delete);
```

Declare `static int g_deleted_count` at file scope. Assert that the lexicographic winner is returned and both losers are deleted exactly once:

```cpp
ok &= ExpectEqual("winner trajectory", result.trajectory_id, 1);
ok &= ExpectEqual("two loser networks freed", deleted_count, 2);
ok &= ExpectEqual("winner metadata balance", result.pNtk->pPdb->balance_pct(), 17);
```

- [ ] **Step 2: Verify the test fails**

```bash
rtk make release
```

Expected: test runner injection or cleanup accounting is unavailable.

- [ ] **Step 3: Implement all three deterministic policies**

Map trajectory IDs exactly:

```cpp
0 -> TrajectoryPolicy::GainFirst
1 -> TrajectoryPolicy::BoundaryConcentration
2 -> TrajectoryPolicy::ScarcityFirst
```

Each trajectory receives fresh `SearchBudget` instances per node or driver-target as specified. Print one summary line per trajectory:

```text
csr: trajectory 1 cut-edge=550 cut-net=60 hop=8 nodes=5300 sec=0.42 valid=1
```

Print the chosen trajectory:

```text
csr: selected trajectory 1
```

On any trajectory audit failure, delete only that trajectory and continue. If all trajectories fail, leave the frame entry network installed.

- [ ] **Step 4: Run deterministic T1 and T3 smoke tests**

```bash
rtk make release
rtk ./release/test_csr
rtk ./release/FoxSYN -c "read regression/SimpleCircuits/mcnc/alu4.v; st; if -K 6; hpart -N 4 --load-part regression/parts_n4_flat/alu4.part; csr -T 1 -v"
rtk ./release/FoxSYN -c "read regression/SimpleCircuits/mcnc/alu4.v; st; if -K 6; hpart -N 4 --load-part regression/parts_n4_flat/alu4.part; csr -T 3 -v"
```

Expected: T1 and T3 succeed; T3 chooses the lexicographic best legal network; repeated runs are identical.

- [ ] **Step 5: Commit**

```bash
rtk git add src/csr/csr_internal.hpp src/csr/csr.cpp src/main.cpp src/test_csr.cpp
rtk git commit -m "Add deterministic CSR multi-trajectory search"
```

## Task 12: Extend regression enforcement, run acceptance tests, and update documentation

**Files:**
- Modify: `scripts/run_csr_regression.py`
- Modify: `docs/csr.md`
- Test: `src/test_csr.cpp`

- [ ] **Step 1: Write failing Python parser tests inline with `unittest`**

Add a `--self-test` mode to `run_csr_regression.py` that feeds representative output to `parse_output` and asserts:

```python
assert parsed.cutnet_before == 56
assert parsed.cutnet_after == 79
assert parsed.selected_trajectory == 1
assert parsed.constraints == "OK"
assert parsed.deterministic is True
```

Refactor output parsing into a pure `parse_output(text: str) -> Result` function so self-tests do not launch FoxSYN.

- [ ] **Step 2: Run the script self-test and verify it fails**

```bash
rtk python3 scripts/run_csr_regression.py --self-test
```

Expected: `--self-test` or `parse_output` is missing.

- [ ] **Step 3: Implement constraint, determinism, baseline, and runtime reporting**

Add fields:

```python
selected_trajectory: str = "-"
constraints: str = "-"
deterministic: bool = False
baseline_cut_after: str = "-"
runtime_ratio: str = "-"
```

Add CLI options:

```text
--exact-repeats 3
--baseline-foxsyn PATH
--baseline-results JSON
--write-results JSON
```

For every successful case, mark `constraints=OK` only when:

```python
hop_after <= hop_before
nodes_after * 100 <= nodes_before * 102
cutnet_after * 100 <= cutnet_before * 150
cut_after <= cut_before
```

`--exact-repeats N` must compare the parsed CSR summaries from all N runs and mark `deterministic=False` on any difference; it must not select the best run.

Compute the default T1 geometric-mean runtime ratio with:

```python
math.exp(sum(math.log(new / old) for old, new in ratios) / len(ratios))
```

- [ ] **Step 4: Run focused, frozen-partition, CEC, determinism, and sanitizer verification**

```bash
rtk make release
rtk ./release/test_hop
rtk ./release/test_cpr
rtk ./release/test_csr
rtk python3 scripts/run_csr_regression.py --self-test
```

From `regression/`, run a two-case frozen regression first:

```bash
rtk ../scripts/run_csr_regression.py --foxsyn ../release/FoxSYN --cases-root SimpleCircuits/mcnc --load-parts-dir parts_n4_flat --match alu4 --csr-args "-T 1 -v" --exact-repeats 3 --cec --timeout 120
rtk ../scripts/run_csr_regression.py --foxsyn ../release/FoxSYN --cases-root SimpleCircuits/EPFL --load-parts-dir parts_n4_flat --match cavlc --csr-args "-T 1 -v" --exact-repeats 3 --cec --timeout 120
```

Expected: both cases are `OK`, `constraints=OK`, `deterministic=True`, and `CEC=EQ`.

Then run the full frozen suite with the timeout formula implemented by the script, followed by ASan on the targeted transaction tests:

```bash
rtk ../scripts/run_csr_regression.py --foxsyn ../release/FoxSYN --baseline-foxsyn /tmp/FoxSYN-csr-baseline-799702c --cases-root SimpleCircuits --load-parts-dir parts_n4_flat --csr-args "-T 1 -v" --exact-repeats 3 --cec --write-results csr_enhanced.json
rtk make asan
rtk ./asan/test_csr
```

Acceptance requires:

- zero constraint violations;
- zero CEC mismatches;
- exact deterministic summaries across three repeats;
- total final cut-edge at least 3% below the recorded current-CSR baseline;
- at least 60% of cases no worse than baseline;
- no baseline-legal case worse by more than 1%;
- default T1 geometric-mean runtime no more than 5×;
- no ASan/UBSan diagnostics.

- [ ] **Step 5: Update `docs/csr.md` with only verified behavior**

Document:

- `-T 1-3`, default 1;
- entry hop, 2% non-refunding shared growth budget, and 150% cut-net cap;
- deterministic policies and winner ordering;
- Phase 0 compound relocation;
- Phase 1 node-level multi-plan search;
- Phase 2 aggregated replication and clusters;
- frame ownership and PDB scalar restoration;
- actual regression totals and runtime measured in Step 4.

Remove the stale statement that Phase 1 runs `Abc_NtkSweep`.

- [ ] **Step 6: Commit**

```bash
rtk git add scripts/run_csr_regression.py docs/csr.md src/test_csr.cpp
rtk git commit -m "Validate enhanced CSR optimization flow"
```

## Final verification checkpoint

- [ ] Run `rtk git status --short` and confirm only intentional files are changed.
- [ ] Run `rtk git log --oneline -12` and confirm every task has its own commit.
- [ ] Run `rtk make release` and all three helper tests.
- [ ] Run the frozen full-suite regression with CEC and exact repeats.
- [ ] Run `rtk make asan` and `rtk ./asan/test_csr`.
- [ ] Compare measured results against every acceptance criterion in the design document.
