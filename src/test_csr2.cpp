#include <climits>
#include <algorithm>
#include <cstdio>
#include <vector>

#include "base/abc/abc.h"
#include "base/abc/abcPdb.hpp"
#include "csr2/csr2.hpp"
#include "csr2/csr2_internal.hpp"

namespace {

int g_deleted_count = 0;

bool ExpectEqual(const char *label, int actual, int expected)
{
    if (actual == expected)
        return true;
    std::fprintf(stderr, "%s: expected %d, got %d\n", label, expected, actual);
    return false;
}

bool ExpectLessEqual(const char *label, int actual, int limit)
{
    if (actual <= limit)
        return true;
    std::fprintf(stderr, "%s: expected <= %d, got %d\n", label, limit, actual);
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

struct CompoundMoveNtk {
    Abc_Ntk_t *pNtk = nullptr;
    Abc_Obj_t *pPi = nullptr;
    Abc_Obj_t *pA = nullptr;
    Abc_Obj_t *pB = nullptr;
    Abc_Obj_t *pSink = nullptr;
    Abc_Obj_t *pPo = nullptr;
};

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

struct ReplicationClusterNtk {
    Abc_Ntk_t *pNtk = nullptr;
    Abc_Obj_t *pD = nullptr;
};

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
    fox::csr2::Config cfg;
    cfg.replicate_growth_pct = 2;
    cfg.cutnet_growth_pct = 150;
    auto limits = fox::csr2::detail::CaptureEntryLimits(t.pNtk, cfg);
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

    fox::csr2::detail::RestorePdbMetadata(pDup, limits);
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
                       fox::csr2::detail::ComputePercentageLimit(INT_MAX, 150, true),
                       INT_MAX);
}

bool TestGrowthBudgetDoesNotRefund()
{
    fox::csr2::detail::GrowthTracker growth(5);
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
    fox::csr2::detail::SearchBudget budget;
    bool ok = true;
    for (int i = 0; i < 128; ++i)
        ok &= ExpectEqual("sat call allowed", budget.TrySatCall(), 1);
    ok &= ExpectEqual("sat call 129 rejected", budget.TrySatCall(), 0);
    return ok;
}

bool TestTrajectoryOrdering()
{
    using fox::csr2::detail::TrajectoryResult;
    TrajectoryResult a{nullptr, {90, 12, 4, 100}, 1, true};
    TrajectoryResult b{nullptr, {90, 11, 4, 101}, 2, true};
    TrajectoryResult c{nullptr, {91, 1, 1, 1}, 0, true};
    bool ok = true;
    ok &= ExpectEqual("lower cutnet wins tie", fox::csr2::detail::BetterResult(b, a), 1);
    ok &= ExpectEqual("cutedge remains primary", fox::csr2::detail::BetterResult(c, a), 0);
    return ok;
}

bool TestCutCandidateTotalOrder()
{
    using fox::csr2::detail::CutCandidate;
    std::vector<CutCandidate> candidates{{9, 1, 5, 2}, {3, 2, 5, 1},
                                         {3, 0, 5, 1}};
    std::sort(candidates.begin(), candidates.end(), fox::csr2::detail::CutCandidateLess{});

    bool ok = true;
    ok &= ExpectEqual("first candidate node", candidates.front().node_id, 3);
    ok &= ExpectEqual("first candidate fanin", candidates.front().iFanin, 0);
    ok &= ExpectEqual("last candidate node", candidates.back().node_id, 9);
    return ok;
}

bool TestDuplicatedTrajectoryUsesCapturedBalance()
{
    StateTestNtk base = CreateStateTestNtk();
    fox::csr2::Config cfg;
    auto limits = fox::csr2::detail::CaptureEntryLimits(base.pNtk, cfg);
    Abc_Ntk_t *pDup = Abc_NtkDup(base.pNtk);
    fox::csr2::detail::OptimizationState state(pDup, limits, 0);
    bool ok = true;
    ok &= ExpectEqual("duplicate Pdb balance invalid", pDup->pPdb->balance_pct(), -1);
    ok &= ExpectEqual("state keeps entry balance", state.limits.balance_pct, 17);
    Abc_NtkDelete(pDup);
    Abc_NtkDelete(base.pNtk);
    return ok;
}

bool TestCompoundRelocation()
{
    CompoundMoveNtk t = CreateCompoundMoveNtk();
    fox::csr2::Config cfg;
    cfg.balance_pct = 99;
    const auto limits = fox::csr2::detail::CaptureEntryLimits(t.pNtk, cfg);
    fox::csr2::detail::OptimizationState state(t.pNtk, limits, 0);
    const int entry_hop = Abc_NtkComputeHopNum(t.pNtk);
    const auto sequence = fox::csr2::detail::FindBestRelocationSequence(
        t.pNtk, state, fox::csr2::detail::TrajectoryPolicy::GainFirst);

    bool ok = true;
    ok &= ExpectEqual("compound move length", static_cast<int>(sequence.steps.size()), 2);
    ok &= ExpectEqual("compound delta", sequence.cutedge_delta, -2);
    ok &= ExpectEqual("apply sequence", fox::csr2::detail::ApplyRelocationSequence(
        t.pNtk, state, sequence), 1);
    ok &= ExpectLessEqual("hop preserved", Abc_NtkComputeHopNum(t.pNtk), entry_hop);
    Abc_NtkDelete(t.pNtk);
    return ok;
}

bool TestHopStateIncreaseAndRollback()
{
    HopPropagationNtk t = CreateHopPropagationNtk();
    const int entry_hop = Abc_NtkComputeHopNum(t.pNtk);
    fox::csr2::detail::HopState hop;
    bool ok = true;
    ok &= ExpectEqual("hop init", hop.Initialize(t.pNtk), 1);
    auto txn = hop.BeginTransaction();
    Abc_ObjPatchFanin(t.pSink, t.pLow, t.pHigh);
    ok &= ExpectEqual("propagate detects limit", hop.PropagateFrom(
        t.pNtk, {t.pSink->Id}, entry_hop, txn), 0);
    hop.Rollback(txn);
    Abc_ObjPatchFanin(t.pSink, t.pHigh, t.pLow);
    ok &= ExpectEqual("rollback matches full", hop.VerifyAgainstFull(t.pNtk), 1);
    Abc_NtkDelete(t.pNtk);
    return ok;
}

bool TestHopStateDecreasePropagation()
{
    HopPropagationNtk t = CreateHopPropagationNtk();
    Abc_Obj_t *pTail1 = Abc_NtkCreateNode(t.pNtk);
    Abc_Obj_t *pTail2 = Abc_NtkCreateNode(t.pNtk);
    Abc_ObjAddFanin(pTail1, t.pSink);
    Abc_ObjAddFanin(pTail2, pTail1);
    SetNodeFunction(pTail1);
    SetNodeFunction(pTail2);
    Abc_ObjSetPartId(pTail1, 0);
    Abc_ObjSetPartId(pTail2, 0);
    Abc_ObjPatchFanin(t.pSink, t.pLow, t.pHigh);

    fox::csr2::detail::HopState hop;
    bool ok = true;
    ok &= ExpectEqual("decrease init", hop.Initialize(t.pNtk), 1);
    ok &= ExpectEqual("decrease entry hop", Abc_NtkComputeHopNum(t.pNtk), 3);
    auto txn = hop.BeginTransaction();
    Abc_ObjPatchFanin(t.pSink, t.pHigh, t.pLow);
    ok &= ExpectEqual("decrease propagate", hop.PropagateFrom(
        t.pNtk, {t.pSink->Id}, 3, txn), 1);
    ok &= ExpectEqual("sink arrival decreases", hop.arrival(t.pSink->Id), 0);
    ok &= ExpectEqual("tail1 arrival decreases", hop.arrival(pTail1->Id), 0);
    ok &= ExpectEqual("tail2 arrival decreases", hop.arrival(pTail2->Id), 0);
    ok &= ExpectEqual("decrease matches full", hop.VerifyAgainstFull(t.pNtk), 1);
    Abc_NtkDelete(t.pNtk);
    return ok;
}

bool TestReplicationCandidateAggregation()
{
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

    const auto candidates = fox::csr2::detail::CollectReplicationCandidates(pNtk);
    bool ok = true;
    ok &= ExpectEqual("one driver-target candidate",
                      static_cast<int>(candidates.size()), 1);
    ok &= ExpectEqual("saved outgoing edges", candidates[0].saved_edges, 3);
    ok &= ExpectEqual("added boundary edges", candidates[0].added_edges, 2);
    ok &= ExpectEqual("net gain", candidates[0].net_gain(), 1);
    Abc_NtkDelete(pNtk);
    return ok;
}

bool TestReplicationClusterTransaction()
{
    auto test = CreateReplicationClusterNtk();
    fox::csr2::Config cfg;
    cfg.balance_pct = 99;
    auto limits = fox::csr2::detail::CaptureEntryLimits(test.pNtk, cfg);
    limits.balance_pct = 150;
    limits.growth_budget = 2;
    limits.node_limit = Abc_NtkNodeNum(test.pNtk) + 2;
    fox::csr2::detail::OptimizationState state(test.pNtk, limits, 0);
    fox::csr2::detail::HopState hop;
    hop.Initialize(test.pNtk);
    auto candidate = fox::csr2::detail::CollectReplicationCandidates(test.pNtk).front();
    auto cluster = fox::csr2::detail::FindBestReplicationCluster(
        test.pNtk, state, candidate, hop);
    bool ok = true;
    ok &= ExpectEqual("cluster size", static_cast<int>(cluster.node_ids.size()), 2);
    ok &= ExpectEqual("cluster positive gain", cluster.cutedge_delta < 0, 1);
    ok &= ExpectEqual("apply cluster", fox::csr2::detail::TryReplicationCluster(
        test.pNtk, state, hop, cluster), 1);
    ok &= ExpectEqual("hop exact", hop.VerifyAgainstFull(test.pNtk), 1);

    auto blocked = CreateReplicationClusterNtk();
    auto blocked_limits = fox::csr2::detail::CaptureEntryLimits(blocked.pNtk, cfg);
    blocked_limits.balance_pct = 150;
    blocked_limits.growth_budget = 2;
    blocked_limits.node_limit = Abc_NtkNodeNum(blocked.pNtk) + 2;
    fox::csr2::detail::OptimizationState blocked_state(blocked.pNtk,
                                                      blocked_limits, 0);
    fox::csr2::detail::HopState blocked_hop;
    blocked_hop.Initialize(blocked.pNtk);
    auto blocked_candidate =
        fox::csr2::detail::CollectReplicationCandidates(blocked.pNtk).front();
    auto blocked_cluster = fox::csr2::detail::FindBestReplicationCluster(
        blocked.pNtk, blocked_state, blocked_candidate, blocked_hop);
    ok &= ExpectEqual("consume all shared growth",
                      blocked_state.growth.TryConsume(blocked_limits.growth_budget), 1);
    const int nodes_before = Abc_NtkNodeNum(blocked.pNtk);
    ok &= ExpectEqual("phase2 rejected after phase1 budget exhaustion",
                      fox::csr2::detail::TryReplicationCluster(
                          blocked.pNtk, blocked_state, blocked_hop,
                          blocked_cluster), 0);
    ok &= ExpectEqual("rejected cluster leaves node count",
                      Abc_NtkNodeNum(blocked.pNtk), nodes_before);
    Abc_NtkDelete(test.pNtk);
    Abc_NtkDelete(blocked.pNtk);
    return ok;
}

bool TestDivisorMetadataAndCutNetDelta()
{
    fox::csr2::detail::DivisorInfo a{3, 1, 4, -1, 0, 3};
    fox::csr2::detail::DivisorInfo b{4, 1, 3, 0, 1, 4};
    std::vector infos{b, a};
    std::sort(infos.begin(), infos.end(), fox::csr2::detail::DivisorInfoLess{});
    bool ok = true;
    ok &= ExpectEqual("coverage first", infos[0].obj_id, 3);

    StateTestNtk t = CreateStateTestNtk();
    std::vector<Abc_Obj_t *> old_fanins{t.pPi0, t.pPi1};
    std::vector<Abc_Obj_t *> new_fanins{t.pPi1};
    const int delta = fox::csr2::detail::ComputeHypotheticalCutNetDelta(
        t.pNode, old_fanins, new_fanins);
    ok &= ExpectEqual("last crossing fanout clears net", delta, -1);
    Abc_NtkDelete(t.pNtk);
    return ok;
}

bool TestResubPlanSelectionAndJointReplacement()
{
    fox::csr2::detail::ResubPlan first;
    first.cutedge_delta = -1;
    first.cutnet_delta = 0;
    first.predicted_hop = 3;
    first.new_fanin_count = 2;
    first.divisor_ids = {8};
    fox::csr2::detail::ResubPlan second;
    second.cutedge_delta = -2;
    second.cutnet_delta = 1;
    second.predicted_hop = 3;
    second.new_fanin_count = 1;
    second.divisor_ids = {9};
    std::vector plans{first, second};
    auto best = fox::csr2::detail::SelectBestResubPlan(plans);
    bool ok = true;
    ok &= ExpectEqual("largest cutedge gain wins", best->divisor_ids[0], 9);
    ok &= ExpectEqual("external divisor reduces two crossings",
                      fox::csr2::detail::ExternalDivisorPlanAllowed(2, 1), 1);
    ok &= ExpectEqual("one for one external replacement rejected",
                      fox::csr2::detail::ExternalDivisorPlanAllowed(1, 1), 0);

    auto t = CreateJointResubNtk();
    fox::csr2::Config cfg;
    cfg.do_relocate = false;
    cfg.replicate_growth_pct = 0;
    auto limits = fox::csr2::detail::CaptureEntryLimits(t.pNtk, cfg);
    limits.balance_pct = 250;
    fox::csr2::detail::OptimizationState state(t.pNtk, limits, 0);
    fox::csr2::detail::Phase1Stats stats;
    ok &= ExpectEqual("joint phase1 succeeds",
                      fox::csr2::detail::RunPhase1Resub(t.pNtk, state, cfg, stats), 1);
    ok &= ExpectEqual("joint replacement counted", stats.joint_replacements, 1);
    ok &= ExpectEqual("consumer now has one fanin",
                      Abc_ObjFaninNum(t.pConsumer), 1);
    ok &= ExpectEqual("external divisor installed",
                      Abc_ObjFanin(t.pConsumer, 0)->Id, t.pDivisor->Id);
    ok &= ExpectEqual("joint resub cutedge",
                      fox::csr2::ComputeCutEdgeCount(t.pNtk), 1);
    ok &= ExpectEqual("joint resub hop", Abc_NtkComputeHopNum(t.pNtk), 1);
    Abc_NtkDelete(t.pNtk);
    return ok;
}

bool TestTrajectoryWinnerCleanup()
{
    StateTestNtk base = CreateStateTestNtk();
    fox::csr2::Config cfg;
    const auto limits = fox::csr2::detail::CaptureEntryLimits(base.pNtk, cfg);
    auto counting_delete = +[](Abc_Ntk_t *pNtk) {
        ++g_deleted_count;
        Abc_NtkDelete(pNtk);
    };
    std::vector<fox::csr2::detail::TrajectoryResult> results = {
        {Abc_NtkDup(base.pNtk), {90, 12, 4, 100}, 0, true},
        {Abc_NtkDup(base.pNtk), {90, 11, 4, 101}, 1, true},
        {Abc_NtkDup(base.pNtk), {91, 1, 1, 1}, 2, true},
    };
    g_deleted_count = 0;
    auto result = fox::csr2::detail::TakeBestTrajectory(results, limits,
                                                       counting_delete);
    bool ok = true;
    ok &= ExpectEqual("winner trajectory", result.trajectory_id, 1);
    ok &= ExpectEqual("two loser networks freed", g_deleted_count, 2);
    ok &= ExpectEqual("winner metadata balance",
                      result.pNtk->pPdb->balance_pct(), 17);
    Abc_NtkDelete(result.pNtk);
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
        && TestDuplicatedTrajectoryUsesCapturedBalance()
        && TestCompoundRelocation()
        && TestHopStateIncreaseAndRollback()
        && TestHopStateDecreasePropagation()
        && TestReplicationCandidateAggregation()
        && TestReplicationClusterTransaction()
        && TestDivisorMetadataAndCutNetDelta()
        && TestResubPlanSelectionAndJointReplacement()
        && TestTrajectoryWinnerCleanup() ? 0 : 1;
    Abc_Stop();
    return result;
}
