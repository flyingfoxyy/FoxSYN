#include <climits>
#include <algorithm>
#include <cstdio>
#include <vector>

#include "base/abc/abc.h"
#include "base/abc/abcPdb.hpp"
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
    ok &= ExpectEqual("fixture nodes", Abc_NtkNodeNum(t.pNtk), 1);
    ok &= ExpectEqual("fixture cut size", Abc_NtkComputeCutSize(t.pNtk), 1);
    ok &= ExpectEqual("fixture hop number", Abc_NtkComputeHopNum(t.pNtk), 1);
    ok &= ExpectEqual("captured num parts", limits.num_parts, 2);
    ok &= ExpectEqual("captured balance", limits.balance_pct, 17);
    ok &= ExpectEqual("captured hop limit", limits.hop_limit, 1);
    ok &= ExpectEqual("captured node limit", limits.node_limit, 2);
    ok &= ExpectEqual("captured growth budget", limits.growth_budget, 0);
    ok &= ExpectEqual("captured cutnet limit", limits.cutnet_limit, 2);
    ok &= ExpectEqual("dup invalidates balance", pDup->pPdb->balance_pct(), -1);

    fox::csr::detail::RestorePdbMetadata(pDup, limits);
    ok &= ExpectEqual("restored balance", pDup->pPdb->balance_pct(), 17);
    ok &= ExpectEqual("restored num parts", pDup->pPdb->num_parts(), 2);
    ok &= ExpectEqual("restored cut size", pDup->pPdb->cut_size(), 1);
    ok &= ExpectEqual("restored hop number", pDup->pPdb->hop_num(), 1);

    Abc_NtkDelete(pDup);
    Abc_NtkDelete(t.pNtk);
    return ok;
}

bool TestPercentageLimitSaturates()
{
    return ExpectEqual("saturated percentage limit",
                       fox::csr::detail::ComputePercentageLimit(INT_MAX, 150, true),
                       INT_MAX);
}

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

bool TestCutCandidateTotalOrder()
{
    using fox::csr::detail::CutCandidate;
    std::vector<CutCandidate> candidates{{9, 1, 5}, {3, 2, 5},
                                         {3, 0, 5}};
    std::sort(candidates.begin(), candidates.end(), fox::csr::detail::CutCandidateLess{});

    bool ok = true;
    ok &= ExpectEqual("first candidate node", candidates.front().node_id, 3);
    ok &= ExpectEqual("first candidate fanin", candidates.front().iFanin, 0);
    ok &= ExpectEqual("last candidate node", candidates.back().node_id, 9);
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

} // namespace

int main()
{
    Abc_Start();
    const int result = TestCaptureEntryLimitsBeforeDup()
        && TestPercentageLimitSaturates()
        && TestGrowthBudgetDoesNotRefund()
        && TestSearchBudgetStopsAtExactLimit()
        && TestTrajectoryOrdering()
        && TestCutCandidateTotalOrder()
        && TestDuplicatedTrajectoryUsesCapturedBalance() ? 0 : 1;
    Abc_Stop();
    return result;
}
