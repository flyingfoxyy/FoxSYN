#include "csr_internal.hpp"

#include "base/abc/abc.h"
#include "base/abc/abcPdb.hpp"
#include "cpr/cpr.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" {
#include "opt/mfs/mfsInt.h"
#include "bool/kit/kit.h"

int Abc_WinNode(Mfs_Man_t *p, Abc_Obj_t *pNode);
int Abc_NtkMfsTryResubOnce(Mfs_Man_t *p, int *pCands, int nCands);
void Abc_NtkMfsUpdateNetwork(Mfs_Man_t *p, Abc_Obj_t *pObj,
                              Vec_Ptr_t *vMfsFanins, Hop_Obj_t *pFunc);
unsigned *Hop_ManConvertAigToTruth(Hop_Man_t *p, Hop_Obj_t *pRoot,
                                    int nVars, Vec_Int_t *vTruth, int fMsbFirst);
Hop_Obj_t *Kit_TruthToHop(Hop_Man_t *pMan, unsigned *pTruth,
                            int nVars, Vec_Int_t *vMemory);
}

namespace fox::csr {

enum ResubReason { RR_SUCCESS = 0, RR_NO_DIV, RR_RESUB_FAIL, RR_OTHER };

static bool is_part_stat_vertex(Abc_Obj_t *pObj)
{
    return pObj
        && (Abc_ObjIsPi(pObj)
         || Abc_ObjIsNode(pObj)
         || Abc_ObjType(pObj) == ABC_OBJ_CONST1);
}

int ComputeCutEdgeCount(Abc_Ntk_t *pNtk)
{
    int count = 0;
    int i, k;
    Abc_Obj_t *pObj, *pFanin;
    Abc_NtkForEachNode(pNtk, pObj, i)
    {
        part_id obj_part = Abc_ObjGetPartId(pObj);
        if (obj_part == ABC_PART_ID_NONE)
            continue;
        Abc_ObjForEachFanin(pObj, pFanin, k)
        {
            if (!is_part_stat_vertex(pFanin))
                continue;
            part_id fanin_part = Abc_ObjGetPartId(pFanin);
            if (fanin_part == ABC_PART_ID_NONE || fanin_part == obj_part)
                continue;
            count++;
        }
    }
    return count;
}

namespace {

bool NetIsCut(Abc_Obj_t *pDriver, part_id driver_part,
              part_id ignored_fanout_part = ABC_PART_ID_NONE)
{
    Abc_Obj_t *pFanout;
    int i;
    Abc_ObjForEachFanout(pDriver, pFanout, i)
    {
        if (!Abc_ObjIsNode(pFanout))
            continue;
        const part_id fanout_part = Abc_ObjGetPartId(pFanout);
        if (fanout_part == ABC_PART_ID_NONE
            || fanout_part == ignored_fanout_part)
            continue;
        if (fanout_part != driver_part)
            return true;
    }
    return false;
}

int PredictReplicationCutnetDelta(Abc_Obj_t *pDriver, part_id target_part)
{
    std::vector<Abc_Obj_t *> affected{pDriver};
    Abc_Obj_t *pFanin;
    int i;
    Abc_ObjForEachFanin(pDriver, pFanin, i)
        if (is_part_stat_vertex(pFanin)
            && Abc_ObjGetPartId(pFanin) != ABC_PART_ID_NONE)
            affected.push_back(pFanin);
    std::sort(affected.begin(), affected.end(),
              [](Abc_Obj_t *lhs, Abc_Obj_t *rhs) { return lhs->Id < rhs->Id; });
    affected.erase(std::unique(affected.begin(), affected.end()), affected.end());

    int before = 0;
    int after = 0;
    for (Abc_Obj_t *pObj : affected)
    {
        const part_id part = Abc_ObjGetPartId(pObj);
        before += NetIsCut(pObj, part);
        if (pObj == pDriver)
            after += NetIsCut(pObj, part, target_part);
        else
            after += NetIsCut(pObj, part) || part != target_part;
    }
    return after - before;
}

bool ReplicationCandidateLess(const detail::ReplicationCandidate &lhs,
                              const detail::ReplicationCandidate &rhs)
{
    const long long lhs_ratio = static_cast<long long>(lhs.net_gain()) * rhs.node_cost;
    const long long rhs_ratio = static_cast<long long>(rhs.net_gain()) * lhs.node_cost;
    if (lhs_ratio != rhs_ratio)
        return lhs_ratio > rhs_ratio;
    return std::tuple{-lhs.net_gain(), lhs.cutnet_delta,
                      lhs.key.driver_id, lhs.key.target_part}
         < std::tuple{-rhs.net_gain(), rhs.cutnet_delta,
                      rhs.key.driver_id, rhs.key.target_part};
}

} // namespace

std::vector<detail::ReplicationCandidate>
detail::CollectReplicationCandidates(Abc_Ntk_t *pNtk)
{
    std::map<std::pair<int, part_id>, int> saved_edges;
    Abc_Obj_t *pDriver, *pFanout;
    int i, k;
    Abc_NtkForEachNode(pNtk, pDriver, i)
    {
        const part_id driver_part = Abc_ObjGetPartId(pDriver);
        if (driver_part == ABC_PART_ID_NONE)
            continue;
        Abc_ObjForEachFanout(pDriver, pFanout, k)
        {
            if (!Abc_ObjIsNode(pFanout))
                continue;
            const part_id target_part = Abc_ObjGetPartId(pFanout);
            if (target_part == ABC_PART_ID_NONE || target_part == driver_part)
                continue;
            saved_edges[{pDriver->Id, target_part}]++;
        }
    }

    std::vector<detail::ReplicationCandidate> candidates;
    for (const auto &[key, saved] : saved_edges)
    {
        pDriver = Abc_NtkObj(pNtk, key.first);
        if (!pDriver || !Abc_ObjIsNode(pDriver))
            continue;
        int added = 0;
        Abc_Obj_t *pFanin;
        Abc_ObjForEachFanin(pDriver, pFanin, k)
            if (is_part_stat_vertex(pFanin)
                && Abc_ObjGetPartId(pFanin) != ABC_PART_ID_NONE
                && Abc_ObjGetPartId(pFanin) != key.second)
                added++;
        candidates.push_back({
            {key.first, key.second},
            saved,
            added,
            PredictReplicationCutnetDelta(pDriver, key.second),
            1,
        });
    }
    std::sort(candidates.begin(), candidates.end(), ReplicationCandidateLess);
    return candidates;
}

int detail::ComputeHypotheticalCutNetDelta(
    Abc_Obj_t *pConsumer,
    const std::vector<Abc_Obj_t *> &old_fanins,
    const std::vector<Abc_Obj_t *> &new_fanins)
{
    if (!pConsumer || Abc_ObjGetPartId(pConsumer) == ABC_PART_ID_NONE)
        return 0;

    std::vector<Abc_Obj_t *> affected = old_fanins;
    affected.insert(affected.end(), new_fanins.begin(), new_fanins.end());
    affected.erase(std::remove(affected.begin(), affected.end(), nullptr),
                   affected.end());
    std::sort(affected.begin(), affected.end(),
              [](Abc_Obj_t *lhs, Abc_Obj_t *rhs) { return lhs->Id < rhs->Id; });
    affected.erase(std::unique(affected.begin(), affected.end()), affected.end());

    const auto contains = [](const std::vector<Abc_Obj_t *> &fanins,
                             Abc_Obj_t *pDriver) {
        return std::find(fanins.begin(), fanins.end(), pDriver) != fanins.end();
    };
    const part_id consumer_part = Abc_ObjGetPartId(pConsumer);
    int before_count = 0;
    int after_count = 0;
    for (Abc_Obj_t *pDriver : affected)
    {
        const part_id driver_part = Abc_ObjGetPartId(pDriver);
        if (!is_part_stat_vertex(pDriver) || driver_part == ABC_PART_ID_NONE)
            continue;

        bool other_cross = false;
        Abc_Obj_t *pFanout;
        int i;
        Abc_ObjForEachFanout(pDriver, pFanout, i)
        {
            if (pFanout == pConsumer || !Abc_ObjIsNode(pFanout))
                continue;
            const part_id fanout_part = Abc_ObjGetPartId(pFanout);
            if (fanout_part != ABC_PART_ID_NONE && fanout_part != driver_part)
            {
                other_cross = true;
                break;
            }
        }
        before_count += other_cross
            || (contains(old_fanins, pDriver) && driver_part != consumer_part);
        after_count += other_cross
            || (contains(new_fanins, pDriver) && driver_part != consumer_part);
    }
    return after_count - before_count;
}

std::optional<detail::ResubPlan> detail::SelectBestResubPlan(
    const std::vector<detail::ResubPlan> &plans)
{
    if (plans.empty())
        return std::nullopt;
    return *std::min_element(plans.begin(), plans.end(),
        [](const auto &lhs, const auto &rhs) {
            return std::tuple{lhs.cutedge_delta, lhs.cutnet_delta,
                              lhs.predicted_hop, lhs.new_fanin_count,
                              lhs.divisor_ids}
                 < std::tuple{rhs.cutedge_delta, rhs.cutnet_delta,
                              rhs.predicted_hop, rhs.new_fanin_count,
                              rhs.divisor_ids};
        });
}

bool detail::ExternalDivisorPlanAllowed(int removed_crossings,
                                        int added_crossings)
{
    return added_crossings < removed_crossings;
}

bool detail::ResubPlanAllowed(const detail::ResubPlan &plan,
                              const detail::OptimizationState &state)
{
    return plan.cutedge_delta < 0
        && state.current.cut_nets + plan.cutnet_delta <= state.limits.cutnet_limit
        && plan.predicted_hop <= state.limits.hop_limit
        && plan.positive_net_growth <= state.growth.remaining();
}

bool detail::RunPhase1Resub(Abc_Ntk_t *pNtk,
                            detail::OptimizationState &state,
                            const Config &, detail::Phase1Stats &stats)
{
    if (!pNtk || state.pNtk != pNtk)
        return false;

    Abc_Obj_t *pConsumer;
    int i;
    Abc_NtkForEachNode(pNtk, pConsumer, i)
    {
        const part_id consumer_part = Abc_ObjGetPartId(pConsumer);
        if (consumer_part == ABC_PART_ID_NONE)
            continue;
        std::vector<Abc_Obj_t *> old_fanins;
        int removed_crossings = 0;
        Abc_Obj_t *pFanin;
        int k;
        Abc_ObjForEachFanin(pConsumer, pFanin, k)
        {
            old_fanins.push_back(pFanin);
            const part_id fanin_part = Abc_ObjGetPartId(pFanin);
            removed_crossings += fanin_part != ABC_PART_ID_NONE
                && fanin_part != consumer_part;
        }
        if (removed_crossings < 2 || old_fanins.size() < 2)
            continue;

        Abc_Obj_t *pDivisor;
        int j;
        Abc_NtkForEachNode(pNtk, pDivisor, j)
        {
            if (pDivisor == pConsumer
                || Abc_ObjFaninNum(pDivisor) != static_cast<int>(old_fanins.size())
                || !pDivisor->pData || !pConsumer->pData)
                continue;
            std::vector<int> divisor_fanins;
            Abc_ObjForEachFanin(pDivisor, pFanin, k)
                divisor_fanins.push_back(pFanin->Id);
            std::vector<int> consumer_fanins;
            for (Abc_Obj_t *pOld : old_fanins)
                consumer_fanins.push_back(pOld->Id);
            std::sort(divisor_fanins.begin(), divisor_fanins.end());
            std::sort(consumer_fanins.begin(), consumer_fanins.end());
            if (divisor_fanins != consumer_fanins)
                continue;
            // pData's type is representation-dependent: a SOP cover string
            // under ABC_FUNC_SOP, but a structurally hashed Hop_Obj_t* under
            // ABC_FUNC_AIG (the representation "if"-mapped networks use).
            // Treating an AIG pData as a string here previously corrupted
            // the SOP memory manager and crashed on every if-mapped input.
            bool same_function;
            if (Abc_NtkHasAig(pNtk))
                same_function = pDivisor->pData == pConsumer->pData;
            else if (Abc_NtkHasSop(pNtk))
                same_function = std::strcmp(static_cast<const char *>(pDivisor->pData),
                                            static_cast<const char *>(pConsumer->pData)) == 0;
            else
                same_function = false;
            if (!same_function)
                continue;

            const int added_crossings =
                Abc_ObjGetPartId(pDivisor) != consumer_part;
            if (!detail::ExternalDivisorPlanAllowed(removed_crossings,
                                                    added_crossings))
                continue;
            stats.attempts++;
            const detail::Metrics before = detail::ComputeMetrics(pNtk);
            std::vector<Abc_Obj_t *> new_fanins{pDivisor};
            const int cutnet_delta = detail::ComputeHypotheticalCutNetDelta(
                pConsumer, old_fanins, new_fanins);
            void *pOldData = pConsumer->pData;
            Abc_ObjRemoveFanins(pConsumer);
            Abc_ObjAddFanin(pConsumer, pDivisor);
            auto *pMan = static_cast<Mem_Flex_t *>(pNtk->pManFunc);
            pConsumer->pData = Abc_SopCreateBuf(pMan);
            const detail::Metrics after = detail::ComputeMetrics(pNtk);
            detail::ResubPlan plan;
            plan.divisor_ids = {pDivisor->Id};
            plan.cutedge_delta = after.cut_edges - before.cut_edges;
            plan.cutnet_delta = cutnet_delta;
            plan.predicted_hop = after.hop;
            plan.new_fanin_count = 1;
            if (detail::ResubPlanAllowed(plan, state))
            {
                state.AttachNetwork(pNtk);
                if (state.Audit())
                {
                    stats.successes++;
                    stats.joint_replacements++;
                    return true;
                }
            }

            Abc_ObjRemoveFanins(pConsumer);
            for (Abc_Obj_t *pOld : old_fanins)
                Abc_ObjAddFanin(pConsumer, pOld);
            pConsumer->pData = pOldData;
            state.AttachNetwork(pNtk);
        }
    }
    return false;
}

// ---------------------------------------------------------------------
// Candidate collection: every (consumer, iFanin) pair whose driver crosses
// into a different partition, weighted by the driver's total number of
// cross-partition fanouts. Scans the whole network (cutsize is structural,
// not path-dependent), unlike cmfs's top-K critical-path scan.
// ---------------------------------------------------------------------

static void collect_cut_candidates(Abc_Ntk_t *pNtk, std::vector<detail::CutCandidate> &candidates)
{
    std::unordered_map<int, int> driver_cross_count;

    int i, k;
    Abc_Obj_t *pObj, *pFanin;
    Abc_NtkForEachNode(pNtk, pObj, i)
    {
        part_id obj_part = Abc_ObjGetPartId(pObj);
        if (obj_part == ABC_PART_ID_NONE)
            continue;
        Abc_ObjForEachFanin(pObj, pFanin, k)
        {
            if (!is_part_stat_vertex(pFanin))
                continue;
            part_id fanin_part = Abc_ObjGetPartId(pFanin);
            if (fanin_part == ABC_PART_ID_NONE || fanin_part == obj_part)
                continue;
            driver_cross_count[pFanin->Id]++;
        }
    }

    candidates.clear();
    Abc_NtkForEachNode(pNtk, pObj, i)
    {
        part_id obj_part = Abc_ObjGetPartId(pObj);
        if (obj_part == ABC_PART_ID_NONE)
            continue;
        Abc_ObjForEachFanin(pObj, pFanin, k)
        {
            if (!is_part_stat_vertex(pFanin))
                continue;
            part_id fanin_part = Abc_ObjGetPartId(pFanin);
            if (fanin_part == ABC_PART_ID_NONE || fanin_part == obj_part)
                continue;
            detail::CutCandidate c;
            c.node_id = pObj->Id;
            c.iFanin  = k;
            c.weight  = driver_cross_count[pFanin->Id];
            candidates.push_back(c);
        }
    }

    std::sort(candidates.begin(), candidates.end(), detail::CutCandidateLess{});
}

// ---------------------------------------------------------------------
// Phase 1: partition-match resub, ported from cmfs.cpp's try_arrival_resub.
// The acceptance criterion is "divisor already lives in the consumer's
// partition" instead of "lower arrival contribution" -- swapping such a
// divisor in adds zero new cross-partition edges.
// ---------------------------------------------------------------------

static part_id choose_partition_csr(Abc_Obj_t **fanins, int n, part_id fallback)
{
    auto count_cross = [&](part_id P) {
        int cross = 0;
        for (int i = 0; i < n; i++)
        {
            part_id fp = Abc_ObjGetPartId(fanins[i]);
            if (fp != ABC_PART_ID_NONE && fp != P)
                cross++;
        }
        return cross;
    };

    part_id best = fallback;
    int bestCross = count_cross(fallback);

    for (int i = 0; i < n; i++)
    {
        part_id P = Abc_ObjGetPartId(fanins[i]);
        if (P == ABC_PART_ID_NONE || P == best)
            continue;
        int cross = count_cross(P);
        if (cross < bestCross)
        {
            bestCross = cross;
            best = P;
        }
    }
    return best;
}

// Shannon decomposition ported from cmfs.cpp's shannon_decompose. csr has no
// arrival signal, so unlike cmfs it always splits on variable 0 -- split
// order only shapes tree depth, while the cut-edge objective is entirely
// handled by choose_partition_csr's per-node placement.
static Abc_Obj_t *shannon_decompose_csr(Abc_Ntk_t *pNtk, unsigned *pTruth, int nVars,
                                        Abc_Obj_t **fanins, part_id partId)
{
    Hop_Man_t *pHop = (Hop_Man_t *)pNtk->pManFunc;

    if (nVars <= 6)
    {
        Abc_Obj_t *pLeaf = Abc_NtkCreateNode(pNtk);
        for (int v = 0; v < nVars; v++)
            Abc_ObjAddFanin(pLeaf, fanins[v]);
        pLeaf->pData = (void *)Kit_TruthToHop(pHop, pTruth, nVars, NULL);
        if (!pLeaf->pData)
            pLeaf->pData = (void *)Hop_ManConst0(pHop);
        part_id P = choose_partition_csr(fanins, nVars, partId);
        Abc_ObjSetPartId(pLeaf, P);
        return pLeaf;
    }

    int iVar = 0;

    int nWords = Kit_TruthWordNum(nVars);
    std::vector<unsigned> cof0(nWords), cof1(nWords), tmp(nWords);
    Kit_TruthCofactor0New(cof0.data(), pTruth, nVars, iVar);
    Kit_TruthCofactor1New(cof1.data(), pTruth, nVars, iVar);

    for (int v = iVar; v < nVars - 1; v++)
    {
        Kit_TruthSwapAdjacentVars(tmp.data(), cof0.data(), nVars, v);
        std::copy(tmp.begin(), tmp.end(), cof0.begin());
        Kit_TruthSwapAdjacentVars(tmp.data(), cof1.data(), nVars, v);
        std::copy(tmp.begin(), tmp.end(), cof1.begin());
    }

    Abc_Obj_t *subFanins[MFS_FANIN_MAX];
    int k = 0;
    for (int v = 0; v < nVars; v++)
        if (v != iVar) { subFanins[k] = fanins[v]; k++; }

    Abc_Obj_t *pN0 = shannon_decompose_csr(pNtk, cof0.data(), nVars - 1, subFanins, partId);
    Abc_Obj_t *pN1 = shannon_decompose_csr(pNtk, cof1.data(), nVars - 1, subFanins, partId);

    Abc_Obj_t *pMux = Abc_NtkCreateNode(pNtk);
    Abc_ObjAddFanin(pMux, fanins[iVar]);
    Abc_ObjAddFanin(pMux, pN1);
    Abc_ObjAddFanin(pMux, pN0);
    pMux->pData = (void *)Hop_Mux(pHop, Hop_IthVar(pHop, 0),
                                   Hop_IthVar(pHop, 1), Hop_IthVar(pHop, 2));
    Abc_Obj_t *muxFanins[3] = { fanins[iVar], pN1, pN0 };
    part_id P = choose_partition_csr(muxFanins, 3, partId);
    Abc_ObjSetPartId(pMux, P);
    return pMux;
}

// Forward hop-arrival snapshot, keyed by object id. arrival[n] = max over
// part_stat fanins of (arrival[fanin] + cross_partition ? 1 : 0). Same metric
// Abc_NtkComputeHopNum maxes over. Computed once per phase1 round; used as the
// per-node hop budget for the resub gate (a resub may not raise the consumer's
// arrival above its snapshot value -- if no node exceeds its snapshot, global
// max hop cannot exceed the round's baseline).
static void csr_hop_arrival(Abc_Ntk_t *pNtk, std::vector<int> &arrival)
{
    arrival.assign(Abc_NtkObjNumMax(pNtk), 0);
    Vec_Ptr_t *vNodes = Abc_NtkDfs(pNtk, 1);
    Abc_Obj_t *pObj;
    int i;
    Vec_PtrForEachEntry(Abc_Obj_t *, vNodes, pObj, i)
    {
        part_id obj_part = Abc_ObjGetPartId(pObj);
        if (obj_part == ABC_PART_ID_NONE)
            continue;
        int best = 0;
        Abc_Obj_t *pFanin;
        int k;
        Abc_ObjForEachFanin(pObj, pFanin, k)
        {
            part_id fp = Abc_ObjGetPartId(pFanin);
            if (fp == ABC_PART_ID_NONE)
                continue;
            int cand = arrival[pFanin->Id] + (fp != obj_part ? 1 : 0);
            if (cand > best)
                best = cand;
        }
        arrival[pObj->Id] = best;
    }
    Vec_PtrFree(vNodes);
}

// Predicted hop-arrival of pNode if its fanin set becomes `fanins` while pNode
// stays in `node_part`. Uses the round-start arrival snapshot for the fanins
// (divisors already existed at round start; a resubbed node only shrinks the
// TFI, so their snapshot arrivals are valid upper-context references).
static int csr_predicted_arrival(const std::vector<int> &arrival, part_id node_part,
                                 Abc_Obj_t **fanins, int n)
{
    int best = 0;
    for (int i = 0; i < n; i++)
    {
        part_id fp = Abc_ObjGetPartId(fanins[i]);
        if (fp == ABC_PART_ID_NONE)
            continue;
        int id = fanins[i]->Id;
        int fa = (static_cast<size_t>(id) < arrival.size()) ? arrival[id] : 0;
        int cand = fa + (fp != node_part ? 1 : 0);
        if (cand > best)
            best = cand;
    }
    return best;
}

static int try_partition_resub(Mfs_Man_t *p, Abc_Obj_t *pNode, int iFanin,
                               int maxTempLut, int *reason,
                               const std::vector<int> &hop_arrival)
{
    int pCands[MFS_FANIN_MAX];
    int nCands = 0;
    Abc_Obj_t *pFanin;
    int i;

    *reason = RR_RESUB_FAIL;

    part_id consumer_part = Abc_ObjGetPartId(pNode);
    int hop_budget = (static_cast<size_t>(pNode->Id) < hop_arrival.size())
                     ? hop_arrival[pNode->Id] : 0;

    Vec_PtrClear(p->vMfsFanins);
    int nDivs = Vec_PtrSize(p->vDivs) - Abc_ObjFaninNum(pNode);
    Abc_ObjForEachFanin(pNode, pFanin, i)
    {
        if (i == iFanin)
            continue;
        Vec_PtrPush(p->vMfsFanins, pFanin);
        int iVar = nDivs + i;
        pCands[nCands++] = Abc_Var2Lit(Vec_IntEntry(p->vProjVarsSat, iVar), 1);
    }

    Vec_PtrFillSimInfo(p->vDivCexes, 0, p->nDivWords);
    p->nCexes = 0;

    int ret = Abc_NtkMfsTryResubOnce(p, pCands, nCands);
    if (ret == 1)
    {
        Hop_Obj_t *pFunc = Abc_NtkMfsInterplate(p, pCands, nCands);
        if (!pFunc)
            { *reason = RR_OTHER; return 0; }
        Abc_NtkMfsUpdateNetwork(p, pNode, p->vMfsFanins, pFunc);
        p->nRemoves++;
        return 1;
    }
    if (ret == -1)
        return 0;

    // Divisors "good" for csr are ones already living in the consumer's
    // partition. No ranking among them (csr has no arrival signal).
    std::vector<int> good_divs;
    for (int d = 0; d < nDivs; d++)
    {
        Abc_Obj_t *pDiv = (Abc_Obj_t *)Vec_PtrEntry(p->vDivs, d);
        part_id div_part = Abc_ObjGetPartId(pDiv);
        if (div_part != ABC_PART_ID_NONE && div_part == consumer_part)
            good_divs.push_back(d);
    }

    if (good_divs.empty())
        { *reason = RR_NO_DIV; return 0; }

    int nWords;
    unsigned *pData;
    int w;

    for (int iter = 0; iter < p->pPars->nWinMax; ++iter)
    {
        nWords = Abc_BitWordNum(p->nCexes);
        if (nWords > p->nDivWords)
            break;

        int found = -1;
        for (int d : good_divs)
        {
            pData = (unsigned *)Vec_PtrEntry(p->vDivCexes, d);
            for (w = 0; w < nWords; w++)
                if (pData[w] != ~(unsigned)0)
                    break;
            if (w == nWords)
            {
                found = d;
                break;
            }
        }
        if (found < 0)
            goto phase4;

        pCands[nCands] = Abc_Var2Lit(Vec_IntEntry(p->vProjVarsSat, found), 1);
        ret = Abc_NtkMfsTryResubOnce(p, pCands, nCands + 1);
        if (ret == 1)
        {
            Hop_Obj_t *pFunc = Abc_NtkMfsInterplate(p, pCands, nCands + 1);
            if (!pFunc)
                { *reason = RR_OTHER; return 0; }
            Vec_PtrPush(p->vMfsFanins, Vec_PtrEntry(p->vDivs, found));
            {
                int nf = Vec_PtrSize(p->vMfsFanins);
                Abc_Obj_t *gf[MFS_FANIN_MAX];
                for (int g = 0; g < nf && g < MFS_FANIN_MAX; g++)
                    gf[g] = (Abc_Obj_t *)Vec_PtrEntry(p->vMfsFanins, g);
                if (csr_predicted_arrival(hop_arrival, consumer_part, gf, nf) > hop_budget)
                    { *reason = RR_RESUB_FAIL; return 0; }
            }
            Abc_NtkMfsUpdateNetwork(p, pNode, p->vMfsFanins, pFunc);
            p->nResubs++;
            return 2;
        }
        if (ret == -1)
            return 0;
        if (p->nCexes >= p->pPars->nWinMax)
            break;
    }

    // Single-divisor resub failed. Try 2-divisor resub if node has room.
    if (Abc_ObjFaninNum(pNode) <= 5 && good_divs.size() >= 2)
    {
    constexpr int MAX_PAIR_DIVS = 30;
    int pair_limit = std::min(static_cast<int>(good_divs.size()), MAX_PAIR_DIVS);

    for (int iter = 0; iter < p->pPars->nWinMax; ++iter)
    {
        nWords = Abc_BitWordNum(p->nCexes);
        if (nWords > p->nDivWords)
            break;

        int found1 = -1, found2 = -1;
        for (int i2 = 1; i2 < pair_limit && found1 < 0; i2++)
        {
            int di = good_divs[i2];
            unsigned *pDi = (unsigned *)Vec_PtrEntry(p->vDivCexes, di);
            for (int j = 0; j < i2; j++)
            {
                int dj = good_divs[j];
                unsigned *pDj = (unsigned *)Vec_PtrEntry(p->vDivCexes, dj);
                for (w = 0; w < nWords; w++)
                    if ((pDi[w] | pDj[w]) != ~(unsigned)0)
                        break;
                if (w == nWords)
                {
                    found1 = dj;
                    found2 = di;
                    break;
                }
            }
        }
        if (found1 < 0)
            goto phase4;

        pCands[nCands]     = Abc_Var2Lit(Vec_IntEntry(p->vProjVarsSat, found1), 1);
        pCands[nCands + 1] = Abc_Var2Lit(Vec_IntEntry(p->vProjVarsSat, found2), 1);
        ret = Abc_NtkMfsTryResubOnce(p, pCands, nCands + 2);
        if (ret == 1)
        {
            Hop_Obj_t *pFunc = Abc_NtkMfsInterplate(p, pCands, nCands + 2);
            if (!pFunc)
                { *reason = RR_OTHER; return 0; }
            Vec_PtrPush(p->vMfsFanins, Vec_PtrEntry(p->vDivs, found1));
            Vec_PtrPush(p->vMfsFanins, Vec_PtrEntry(p->vDivs, found2));
            {
                int nf = Vec_PtrSize(p->vMfsFanins);
                Abc_Obj_t *gf[MFS_FANIN_MAX];
                for (int g = 0; g < nf && g < MFS_FANIN_MAX; g++)
                    gf[g] = (Abc_Obj_t *)Vec_PtrEntry(p->vMfsFanins, g);
                if (csr_predicted_arrival(hop_arrival, consumer_part, gf, nf) > hop_budget)
                    { *reason = RR_RESUB_FAIL; return 0; }
            }
            Abc_NtkMfsUpdateNetwork(p, pNode, p->vMfsFanins, pFunc);
            p->nResubs++;
            return 3;
        }
        if (ret == -1)
            return 0;
        if (p->nCexes >= p->pPars->nWinMax)
            break;
    }
    }

phase4:
    if (maxTempLut < 7 || nCands + 3 > maxTempLut)
        return 0;
    if (good_divs.size() < 3)
        return 0;

    int maxExtra = std::min(maxTempLut - nCands, static_cast<int>(good_divs.size()));
    maxExtra = std::min(maxExtra, MFS_FANIN_MAX - nCands);
    maxExtra = std::min(maxExtra, 8 - nCands);

    for (int k = 0; k < maxExtra; ++k)
    {
        nWords = Abc_BitWordNum(p->nCexes);
        if (nWords > p->nDivWords)
            break;

        int found = -1;
        for (int gi = 0; gi < static_cast<int>(good_divs.size()); gi++)
        {
            int d = good_divs[gi];
            bool already = false;
            for (int c = nCands; c < nCands + k; c++)
                if (pCands[c] == Abc_Var2Lit(Vec_IntEntry(p->vProjVarsSat, d), 1))
                { already = true; break; }
            if (already) continue;

            pData = (unsigned *)Vec_PtrEntry(p->vDivCexes, d);
            for (w = 0; w < nWords; w++)
                if (pData[w] != ~(unsigned)0) break;
            if (w < nWords) { found = d; break; }
        }
        if (found < 0)
            return 0;

        pCands[nCands + k] = Abc_Var2Lit(Vec_IntEntry(p->vProjVarsSat, found), 1);
        ret = Abc_NtkMfsTryResubOnce(p, pCands, nCands + k + 1);
        if (ret == 1)
        {
            Hop_Obj_t *pFunc = Abc_NtkMfsInterplate(p, pCands, nCands + k + 1);
            if (!pFunc) { *reason = RR_OTHER; return 0; }

            Vec_PtrClear(p->vMfsFanins);
            Abc_Obj_t *pFi; int fi;
            Abc_ObjForEachFanin(pNode, pFi, fi)
                if (fi != iFanin)
                    Vec_PtrPush(p->vMfsFanins, pFi);
            for (int c = nCands; c <= nCands + k; c++)
            {
                int var = Abc_Lit2Var(pCands[c]);
                for (int d = 0; d < Vec_IntSize(p->vProjVarsSat); d++)
                    if (Vec_IntEntry(p->vProjVarsSat, d) == var)
                    { Vec_PtrPush(p->vMfsFanins, Vec_PtrEntry(p->vDivs, d)); break; }
            }

            int nTotal = Vec_PtrSize(p->vMfsFanins);
            if (nTotal <= 6)
            {
                Abc_NtkMfsUpdateNetwork(p, pNode, p->vMfsFanins, pFunc);
                p->nResubs++;
                return 4;
            }

            Hop_Man_t *pHopMan = (Hop_Man_t *)pNode->pNtk->pManFunc;
            Vec_Int_t *vTruth = Vec_IntAlloc(Kit_TruthWordNum(nTotal) * 2 + 32);
            unsigned *pTruth = Hop_ManConvertAigToTruth(pHopMan, pFunc, nTotal, vTruth, 0);
            if (!pTruth) { Vec_IntFree(vTruth); *reason = RR_OTHER; return 0; }

            int nTtWords = Kit_TruthWordNum(nTotal);
            std::vector<unsigned> ttCopy(pTruth, pTruth + nTtWords);
            Vec_IntFree(vTruth);

            Abc_Obj_t *decomp_fanins[MFS_FANIN_MAX];
            for (int j = 0; j < nTotal; j++)
                decomp_fanins[j] = (Abc_Obj_t *)Vec_PtrEntry(p->vMfsFanins, j);

            part_id partId = Abc_ObjGetPartId(pNode);
            Abc_Obj_t *pRoot = shannon_decompose_csr(pNode->pNtk, ttCopy.data(), nTotal,
                                                      decomp_fanins, partId);
            Abc_ObjSetPartId(pRoot, partId);
            Abc_ObjTransferFanout(pNode, pRoot);
            Abc_NtkDeleteObj_rec(pNode, 1);
            p->nResubs++;
            return 4;
        }
        if (ret == -1)
            return 0;
        if (p->nCexes >= p->pPars->nWinMax)
            break;
    }
    return 0;
}

static void run_phase1_resub(Abc_Ntk_t *&pNtk, const Config &cfg,
                             detail::OptimizationState &state,
                             int &total_attempts, int &total_successes)
{
    int best_cutedges = ComputeCutEdgeCount(pNtk);
    int stall_count = 0;

    for (int round = 0; round < cfg.max_rounds; ++round)
    {
        Abc_Ntk_t *pSnapshot = Abc_NtkDup(pNtk);
        if (!pSnapshot)
            break;
        const int nodes_before = Abc_NtkNodeNum(pSnapshot);
        Abc_NtkToSop(pNtk, -1, ABC_INFINITY);
        if (!Abc_NtkToAig(pNtk))
        {
            Abc_NtkDelete(pSnapshot);
            break;
        }

        std::vector<detail::CutCandidate> candidates;
        collect_cut_candidates(pNtk, candidates);
        if (candidates.empty())
        {
            Abc_NtkDelete(pSnapshot);
            break;
        }

        Abc_NtkLevel(pNtk);
        Abc_NtkStartReverseLevels(pNtk, 0);

        std::vector<int> hop_arrival;
        csr_hop_arrival(pNtk, hop_arrival);

        Mfs_Par_t pars;
        Abc_NtkMfsParsDefault(&pars);
        pars.fResub      = 1;
        pars.nBTLimit    = cfg.nBTLimit;
        pars.nWinTfoLevs = cfg.nWinTfoLevs;
        pars.nFanoutsMax = cfg.nFanoutsMax;
        pars.nWinMax     = cfg.nWinMax;

        Mfs_Man_t *p = Mfs_ManAlloc(&pars);
        p->pNtk = pNtk;
        p->nFaninMax = std::min(Abc_NtkGetFaninMax(pNtk), MFS_FANIN_MAX);

        int round_successes = 0;
        std::vector<char> node_done(Abc_NtkObjNumMax(pNtk), 0);

        for (const auto &cand : candidates)
        {
            if (static_cast<size_t>(cand.node_id) < node_done.size()
                && node_done[cand.node_id])
                continue;

            Abc_Obj_t *pNode = Abc_NtkObj(pNtk, cand.node_id);
            if (!pNode || !Abc_ObjIsNode(pNode))
                continue;
            if (cand.iFanin >= Abc_ObjFaninNum(pNode))
                continue;
            if (Abc_ObjFaninNum(pNode) > p->nFaninMax)
                continue;

            part_id orig_part = Abc_ObjGetPartId(pNode);
            int id_before = Abc_NtkObjNumMax(pNtk);

            total_attempts++;
            int reason = RR_OTHER;

            if (Abc_WinNode(p, pNode) != 0)
                continue;

            int ret = try_partition_resub(p, pNode, cand.iFanin, cfg.maxTempLut, &reason,
                                          hop_arrival);

            if (ret >= 1)
            {
                total_successes++;
                round_successes++;
                Abc_Obj_t *pNew = Abc_NtkObj(pNtk, id_before);
                if (pNew && orig_part != ABC_PART_ID_NONE)
                    Abc_ObjSetPartId(pNew, orig_part);
                if (static_cast<size_t>(cand.node_id) < node_done.size())
                    node_done[cand.node_id] = 1;
            }
        }

        Mfs_ManStop(p);
        Abc_NtkStopReverseLevels(pNtk);

        // Abc_NtkSweep is skipped here: it converts to BDD and patches out
        // any <2-fanin node via Abc_ObjPatchFanin, which would delete
        // pdecomp's cross-partition identity buffers (see pdecomp.cpp) and
        // reopen the fanout-explosion bug they exist to prevent, if csr
        // runs after pdecomp. Abc_NtkCleanup only drops nodes unreachable
        // from the POs and leaves live single-input nodes alone.
        Abc_NtkCleanup(pNtk, 0);

        const int growth = std::max(0, Abc_NtkNodeNum(pNtk) - nodes_before);
        if (state.growth.remaining() < growth || !state.Audit())
        {
            Abc_NtkDelete(pNtk);
            pNtk = pSnapshot;
            state.AttachNetwork(pNtk);
            if (cfg.verbose)
                printf("csr: phase1 round %2d rolled back by hard constraints\n", round);
            break;
        }
        state.growth.TryConsume(growth);
        Abc_NtkDelete(pSnapshot);

        int new_cutedges = ComputeCutEdgeCount(pNtk);
        if (cfg.verbose)
            printf("csr: phase1 round %2d  candidates=%3zu  fixed=%3d  cut-edges=%d\n",
                   round, candidates.size(), round_successes, new_cutedges);

        if (new_cutedges < best_cutedges)
        {
            best_cutedges = new_cutedges;
            stall_count = 0;
        }
        else
        {
            stall_count++;
            if (stall_count >= cfg.stall_limit)
                break;
        }

        if (round_successes == 0)
            break;
    }
}

// ---------------------------------------------------------------------
// Phase 2: replication fallback, ported from cpr.cpp's duplicate_node /
// try_replicate_on_path. Accept criterion is a strict cut-edge decrease
// AND a hop-slack check (see HopSlack below): One replication repoints
// every fanout of the driver already living in the target partition, not
// just the candidate edge that triggered it.
// ---------------------------------------------------------------------

static Abc_Obj_t *duplicate_node_csr(Abc_Ntk_t *pNtk, Abc_Obj_t *pObj)
{
    Abc_Obj_t *pDup = Abc_NtkDupObj(pNtk, pObj, 0);
    if (!pDup)
        return nullptr;
    Abc_Obj_t *pFanin;
    int i;
    Abc_ObjForEachFanin(pObj, pFanin, i)
        Abc_ObjAddFanin(pDup, pFanin);
    return pDup;
}

namespace {

constexpr int kMaxClusterDepth = 2;
constexpr int kMaxClusterNodes = 3;

struct ClusterPatch {
    Abc_Obj_t *pConsumer = nullptr;
    Abc_Obj_t *pOldDriver = nullptr;
    Abc_Obj_t *pNewDriver = nullptr;
};

struct ClusterTransaction {
    std::vector<ClusterPatch> patches;
    std::vector<Abc_Obj_t *> duplicates;
};

void RollbackCluster(Abc_Ntk_t *pNtk, ClusterTransaction &txn)
{
    for (auto it = txn.patches.rbegin(); it != txn.patches.rend(); ++it)
        Abc_ObjPatchFanin(it->pConsumer, it->pNewDriver, it->pOldDriver);
    for (auto it = txn.duplicates.rbegin(); it != txn.duplicates.rend(); ++it)
        Abc_NtkDeleteObj(*it);
    txn.patches.clear();
    txn.duplicates.clear();
}

std::vector<int> OrderClusterNodes(const detail::ReplicationCluster &cluster,
                                   const detail::HopState &hop)
{
    std::vector<int> ordered = cluster.node_ids;
    std::sort(ordered.begin(), ordered.end(), [&](int lhs, int rhs) {
        const int lhs_rank = hop.topo_rank(lhs);
        const int rhs_rank = hop.topo_rank(rhs);
        return std::tuple{lhs_rank, lhs} < std::tuple{rhs_rank, rhs};
    });
    return ordered;
}

bool BuildCluster(Abc_Ntk_t *pNtk, const detail::ReplicationCluster &cluster,
                  const detail::HopState &hop, ClusterTransaction &txn)
{
    txn = {};
    if (cluster.node_ids.empty())
        return false;

    const std::vector<int> ordered = OrderClusterNodes(cluster, hop);
    std::map<int, Abc_Obj_t *> duplicates;
    for (int node_id : ordered)
    {
        Abc_Obj_t *pOriginal = Abc_NtkObj(pNtk, node_id);
        if (!pOriginal || !Abc_ObjIsNode(pOriginal))
        {
            RollbackCluster(pNtk, txn);
            return false;
        }
        Abc_Obj_t *pDup = duplicate_node_csr(pNtk, pOriginal);
        if (!pDup)
        {
            RollbackCluster(pNtk, txn);
            return false;
        }
        Abc_ObjSetPartId(pDup, cluster.key.target_part);
        duplicates[node_id] = pDup;
        txn.duplicates.push_back(pDup);

        Abc_Obj_t *pFanin;
        int i;
        Abc_ObjForEachFanin(pOriginal, pFanin, i)
        {
            auto it = duplicates.find(pFanin->Id);
            if (it != duplicates.end())
                Abc_ObjPatchFanin(pDup, pFanin, it->second);
        }
    }

    auto root_it = duplicates.find(cluster.key.driver_id);
    Abc_Obj_t *pRoot = Abc_NtkObj(pNtk, cluster.key.driver_id);
    if (!pRoot || root_it == duplicates.end())
    {
        RollbackCluster(pNtk, txn);
        return false;
    }

    std::vector<Abc_Obj_t *> target_fanouts;
    Abc_Obj_t *pFanout;
    int i;
    Abc_ObjForEachFanout(pRoot, pFanout, i)
        if (Abc_ObjIsNode(pFanout)
            && Abc_ObjGetPartId(pFanout) == cluster.key.target_part)
            target_fanouts.push_back(pFanout);
    if (target_fanouts.empty())
    {
        RollbackCluster(pNtk, txn);
        return false;
    }
    for (Abc_Obj_t *pConsumer : target_fanouts)
    {
        Abc_ObjPatchFanin(pConsumer, pRoot, root_it->second);
        txn.patches.push_back({pConsumer, pRoot, root_it->second});
    }
    return true;
}

bool ClusterLimitsHold(Abc_Ntk_t *pNtk,
                       const detail::OptimizationState &state,
                       const detail::Metrics &metrics)
{
    if (metrics.hop > state.limits.hop_limit
        || metrics.nodes > state.limits.node_limit
        || metrics.cut_nets > state.limits.cutnet_limit)
        return false;
    std::vector<int> part_sizes;
    fox::cpr::partition_sizes(pNtk, state.limits.num_parts, part_sizes);
    const int max_allowed = fox::cpr::compute_balance_max_allowed(
        part_sizes, state.limits.balance_pct);
    return fox::cpr::compute_balance_overflow(part_sizes, max_allowed)
        <= state.limits.balance_overflow_limit;
}

bool EvaluateCluster(Abc_Ntk_t *pNtk, detail::OptimizationState &state,
                     detail::HopState &hop, detail::ReplicationCluster &cluster)
{
    if (cluster.node_ids.empty()
        || static_cast<int>(cluster.node_ids.size()) > kMaxClusterNodes
        || state.growth.remaining() < static_cast<int>(cluster.node_ids.size()))
        return false;
    const detail::Metrics before = detail::ComputeMetrics(pNtk);
    ClusterTransaction txn;
    if (!BuildCluster(pNtk, cluster, hop, txn))
    {
        hop.Initialize(pNtk);
        return false;
    }

    const detail::Metrics after = detail::ComputeMetrics(pNtk);
    detail::HopState tentative_hop = hop;
    const bool hop_ok = tentative_hop.Initialize(pNtk)
        && after.hop <= state.limits.hop_limit;
    cluster.cutedge_delta = after.cut_edges - before.cut_edges;
    cluster.cutnet_delta = after.cut_nets - before.cut_nets;
    cluster.positive_net_growth = static_cast<int>(cluster.node_ids.size());
    const bool legal = cluster.cutedge_delta < 0 && hop_ok
        && ClusterLimitsHold(pNtk, state, after);
    RollbackCluster(pNtk, txn);
    hop.Initialize(pNtk);
    return legal;
}

bool ClusterLess(const detail::ReplicationCluster &lhs,
                 const detail::ReplicationCluster &rhs)
{
    return std::tuple{lhs.cutedge_delta, lhs.node_ids.size(), lhs.cutnet_delta,
                      lhs.node_ids}
         < std::tuple{rhs.cutedge_delta, rhs.node_ids.size(), rhs.cutnet_delta,
                      rhs.node_ids};
}

} // namespace

detail::ReplicationCluster detail::FindBestReplicationCluster(
    Abc_Ntk_t *pNtk, detail::OptimizationState &state,
    const detail::ReplicationCandidate &candidate, detail::HopState &hop)
{
    detail::ReplicationCluster best;
    if (!pNtk || state.pNtk != pNtk)
        return best;

    std::vector<std::vector<int>> frontier{{candidate.key.driver_id}};
    int evaluated = 0;
    for (int depth = 0; depth <= kMaxClusterDepth && !frontier.empty(); ++depth)
    {
        std::vector<std::vector<int>> next;
        for (auto node_ids : frontier)
        {
            std::sort(node_ids.begin(), node_ids.end());
            node_ids.erase(std::unique(node_ids.begin(), node_ids.end()),
                           node_ids.end());
            detail::ReplicationCluster cluster{candidate.key, node_ids};
            if (evaluated++ < detail::SearchBudget::kMaxClustersPerDriverPart
                && EvaluateCluster(pNtk, state, hop, cluster)
                && (best.node_ids.empty() || ClusterLess(cluster, best)))
                best = cluster;
            if (depth == kMaxClusterDepth
                || static_cast<int>(node_ids.size()) >= kMaxClusterNodes)
                continue;

            for (int node_id : node_ids)
            {
                Abc_Obj_t *pNode = Abc_NtkObj(pNtk, node_id);
                if (!pNode)
                    continue;
                Abc_Obj_t *pFanin;
                int i;
                Abc_ObjForEachFanin(pNode, pFanin, i)
                {
                    if (!Abc_ObjIsNode(pFanin)
                        || Abc_ObjGetPartId(pFanin) == candidate.key.target_part
                        || std::find(node_ids.begin(), node_ids.end(), pFanin->Id)
                            != node_ids.end())
                        continue;
                    auto expanded = node_ids;
                    expanded.push_back(pFanin->Id);
                    std::sort(expanded.begin(), expanded.end());
                    if (std::find(next.begin(), next.end(), expanded) == next.end())
                        next.push_back(std::move(expanded));
                }
            }
        }
        frontier = std::move(next);
    }
    if (!best.node_ids.empty())
    {
        detail::ReplicationCluster ordered = best;
        ordered.node_ids = OrderClusterNodes(best, hop);
        return ordered;
    }
    return best;
}

bool detail::TryReplicationCluster(Abc_Ntk_t *pNtk,
                                   detail::OptimizationState &state,
                                   detail::HopState &hop,
                                   const detail::ReplicationCluster &cluster)
{
    const int growth = static_cast<int>(cluster.node_ids.size());
    if (!pNtk || state.pNtk != pNtk || growth <= 0
        || growth > kMaxClusterNodes || state.growth.remaining() < growth)
        return false;

    const detail::Metrics before = detail::ComputeMetrics(pNtk);
    ClusterTransaction txn;
    if (!BuildCluster(pNtk, cluster, hop, txn))
    {
        hop.Initialize(pNtk);
        return false;
    }

    const detail::Metrics after = detail::ComputeMetrics(pNtk);
    const int cutedge_delta = after.cut_edges - before.cut_edges;
    const int cutnet_delta = after.cut_nets - before.cut_nets;
    const bool legal = cutedge_delta < 0
        && cutedge_delta == cluster.cutedge_delta
        && cutnet_delta == cluster.cutnet_delta
        && hop.Initialize(pNtk)
        && ClusterLimitsHold(pNtk, state, after);
    if (legal)
    {
        state.AttachNetwork(pNtk);
        if (state.Audit() && state.growth.TryConsume(growth))
            return true;
    }

    RollbackCluster(pNtk, txn);
    hop.Initialize(pNtk);
    state.AttachNetwork(pNtk);
    return false;
}

// Hop-slack snapshot, computed once before Phase 2 starts (the network is
// stable at that point -- Phase 1 has already converged). Same shape as
// timing slack: hop_arrival is a forward DFS max over fanins (+1 per
// cross-partition fanin edge), hop_required is a backward pass from POs
// (required = max_hop at POs, min over fanouts of required-crossing
// upstream), and slack = required - arrival. slack==0 means the node sits
// on some longest hop path and has zero room to grow.
struct HopSlack {
    std::vector<int> arrival;
    std::vector<int> required;
    int max_hop = 0;

    int slack(int id) const
    {
        if (id < 0 || static_cast<size_t>(id) >= arrival.size())
            return 0;
        return required[id] - arrival[id];
    }
};

static void compute_hop_slack(Abc_Ntk_t *pNtk, HopSlack &hs)
{
    int n = Abc_NtkObjNumMax(pNtk);
    hs.arrival.assign(n, 0);
    hs.required.assign(n, 0);
    hs.max_hop = 0;

    Vec_Ptr_t *vNodes = Abc_NtkDfs(pNtk, 1);
    Abc_Obj_t *pObj;
    int i;

    Vec_PtrForEachEntry(Abc_Obj_t *, vNodes, pObj, i)
    {
        part_id obj_part = Abc_ObjGetPartId(pObj);
        if (obj_part == ABC_PART_ID_NONE)
            continue;
        int best = 0;
        Abc_Obj_t *pFanin;
        int k;
        Abc_ObjForEachFanin(pObj, pFanin, k)
        {
            part_id fanin_part = Abc_ObjGetPartId(pFanin);
            if (fanin_part == ABC_PART_ID_NONE)
                continue;
            int cand = hs.arrival[pFanin->Id] + (fanin_part != obj_part ? 1 : 0);
            if (cand > best)
                best = cand;
        }
        hs.arrival[pObj->Id] = best;
        if (best > hs.max_hop)
            hs.max_hop = best;
    }

    // Backward pass in reverse topological order: required(n) = min over
    // fanouts g of { required(g) - (cross_partition(n,g) ? 1 : 0) }.
    // POs are not part_stat vertices, so their drivers get max_hop directly
    // (matching Abc_NtkComputeHopNum's PO-exclusion convention).
    hs.required.assign(n, hs.max_hop);
    Vec_PtrForEachEntryReverse(Abc_Obj_t *, vNodes, pObj, i)
    {
        part_id obj_part = Abc_ObjGetPartId(pObj);
        if (obj_part == ABC_PART_ID_NONE)
            continue;
        int best_req = hs.max_hop;
        bool has_fanout = false;
        Abc_Obj_t *pFanout;
        int k;
        Abc_ObjForEachFanout(pObj, pFanout, k)
        {
            if (!is_part_stat_vertex(pFanout))
                continue;
            part_id fanout_part = Abc_ObjGetPartId(pFanout);
            if (fanout_part == ABC_PART_ID_NONE)
                continue;
            int req = hs.required[pFanout->Id] - (fanout_part != obj_part ? 1 : 0);
            if (!has_fanout || req < best_req)
                { best_req = req; has_fanout = true; }
        }
        hs.required[pObj->Id] = has_fanout ? best_req : hs.max_hop;
    }

    Vec_PtrFree(vNodes);
}

static bool try_replicate(Abc_Ntk_t *pNtk, Abc_Obj_t *pDriver, part_id target_part,
                          const HopSlack &hs, detail::OptimizationState &state,
                          int &cur_cutedges)
{
    // Only logic nodes can be duplicated. A PI/CONST1 driver is a single
    // physical pin/constant in the design; "duplicating" it would fabricate
    // a second primary input, corrupting the network (matches cpr's
    // restriction in ordered_path_nodes/try_replicate_on_path).
    if (!Abc_ObjIsNode(pDriver))
        return false;
    if (state.growth.remaining() < 1)
        return false;

    part_id driver_part  = Abc_ObjGetPartId(pDriver);
    if (target_part == ABC_PART_ID_NONE || driver_part == ABC_PART_ID_NONE
        || target_part == driver_part)
        return false;

    // Hop-slack gate: the duplicate's own hop arrival (recomputed from its
    // fanins under the *new* target partition) must not exceed the
    // original driver's hop budget (its arrival + slack). This is a local
    // check -- O(fanins), no network-wide recompute -- because replication
    // only ever changes the driver-side crossing count of the duplicate's
    // own fanin edges; it does not alter any other node's fanins.
    int allowed = hs.arrival[pDriver->Id] + hs.slack(pDriver->Id);
    int new_arr = 0;
    Abc_Obj_t *pFanin;
    int fi;
    Abc_ObjForEachFanin(pDriver, pFanin, fi)
    {
        part_id fanin_part = Abc_ObjGetPartId(pFanin);
        if (fanin_part == ABC_PART_ID_NONE)
            continue;
        int cand = hs.arrival[pFanin->Id] + (fanin_part != target_part ? 1 : 0);
        if (cand > new_arr)
            new_arr = cand;
    }
    if (new_arr > allowed)
        return false;

    std::vector<Abc_Obj_t *> fanouts;
    Abc_Obj_t *pFanout;
    int i;
    Abc_ObjForEachFanout(pDriver, pFanout, i)
        if (Abc_ObjGetPartId(pFanout) == target_part)
            fanouts.push_back(pFanout);
    if (fanouts.empty())
        return false;

    Abc_Obj_t *pDup = duplicate_node_csr(pNtk, pDriver);
    if (!pDup)
        return false;
    Abc_ObjSetPartId(pDup, target_part);

    for (Abc_Obj_t *pF : fanouts)
        Abc_ObjPatchFanin(pF, pDriver, pDup);

    int new_cutedges = ComputeCutEdgeCount(pNtk);
    if (new_cutedges < cur_cutedges && state.Audit() && state.growth.TryConsume(1))
    {
        cur_cutedges = new_cutedges;
        return true;
    }

    for (Abc_Obj_t *pF : fanouts)
        Abc_ObjPatchFanin(pF, pDup, pDriver);
    Abc_NtkDeleteObj(pDup);
    state.Audit();
    return false;
}

static void run_phase2_replicate(Abc_Ntk_t *pNtk, const Config &cfg,
                                 detail::OptimizationState &state,
                                 int &total_replications)
{
    int cur_cutedges = ComputeCutEdgeCount(pNtk);
    int stall_count = 0;

    detail::HopState hop;
    if (!hop.Initialize(pNtk))
        return;

    for (int round = 0; round < cfg.max_rounds; ++round)
    {
        if (state.growth.remaining() < 1)
        {
            if (cfg.verbose)
                printf("csr: phase2 node budget exhausted (%d/%d), stopping\n",
                       state.growth.used(), state.limits.growth_budget);
            break;
        }

        std::vector<detail::ReplicationCandidate> candidates =
            detail::CollectReplicationCandidates(pNtk);
        if (candidates.empty())
            break;

        int round_fixed = 0;
        for (const auto &cand : candidates)
        {
            if (state.growth.remaining() < 1)
                break;

            const auto cluster = detail::FindBestReplicationCluster(
                pNtk, state, cand, hop);
            if (!cluster.node_ids.empty()
                && detail::TryReplicationCluster(pNtk, state, hop, cluster))
            {
                cur_cutedges = ComputeCutEdgeCount(pNtk);
                round_fixed++;
            }
        }

        if (!hop.VerifyAgainstFull(pNtk))
        {
            if (cfg.verbose)
                printf("csr: phase2 incremental hop verification failed\n");
            break;
        }

        if (cfg.verbose)
            printf("csr: phase2 round %2d  candidates=%3zu  replicated=%3d"
                   "  cut-edges=%d  nodes=%d/%d\n",
                   round, candidates.size(), round_fixed, cur_cutedges,
                   state.growth.used(), state.limits.growth_budget);

        total_replications += round_fixed;

        if (round_fixed == 0)
        {
            if (++stall_count >= cfg.stall_limit)
                break;
        }
        else
        {
            stall_count = 0;
        }
    }
}

// Count pNode's incident cross-partition cut-edges assuming it lives in
// `as_part`. Fanin edges: pNode is the consumer (a node), so any part_stat
// driver in a different partition is a cut-edge. Fanout edges: pNode is the
// driver, so only NODE consumers in a different partition count (matches
// ComputeCutEdgeCount's Abc_NtkForEachNode outer loop; PO fanouts never
// count). Since only pNode's part changes on a move, this equals the exact
// global cut-edge contribution of pNode.
static int node_incident_cross(Abc_Obj_t *pNode, part_id as_part)
{
    int cross = 0;
    Abc_Obj_t *pObj;
    int k;
    Abc_ObjForEachFanin(pNode, pObj, k)
    {
        if (!is_part_stat_vertex(pObj))
            continue;
        part_id fp = Abc_ObjGetPartId(pObj);
        if (fp != ABC_PART_ID_NONE && fp != as_part)
            cross++;
    }
    Abc_ObjForEachFanout(pNode, pObj, k)
    {
        if (!Abc_ObjIsNode(pObj))
            continue;
        part_id fp = Abc_ObjGetPartId(pObj);
        if (fp != ABC_PART_ID_NONE && fp != as_part)
            cross++;
    }
    return cross;
}

// Pick the neighbor partition (a partition some fanin/fanout lives in) that
// minimizes pNode's incident cross-edges. Returns the current partition with
// best_delta=0 if no neighbor partition is strictly better. best_delta is the
// exact global cut-edge change of the returned move (<0 means improvement).
static part_id best_relocate_target(Abc_Obj_t *pNode, int &best_delta)
{
    part_id cur = Abc_ObjGetPartId(pNode);
    int cur_cross = node_incident_cross(pNode, cur);
    part_id best = cur;
    int best_cross = cur_cross;

    auto consider = [&](part_id P) {
        if (P == ABC_PART_ID_NONE || P == cur)
            return;
        int c = node_incident_cross(pNode, P);
        if (c < best_cross)
        {
            best_cross = c;
            best = P;
        }
    };

    Abc_Obj_t *pObj;
    int k;
    Abc_ObjForEachFanin(pNode, pObj, k)
        consider(Abc_ObjGetPartId(pObj));
    Abc_ObjForEachFanout(pNode, pObj, k)
        consider(Abc_ObjGetPartId(pObj));

    best_delta = best_cross - cur_cross;
    return best;
}

namespace {

constexpr int kRelocationSeedLimit = 64;
constexpr int kRelocationBeamWidth = 8;
constexpr int kRelocationMaxDepth = 3;

struct RelocationCandidate {
    detail::RelocationStep step;
    int local_delta = 0;
    int target_size = 0;
};

void RollbackRelocation(Abc_Ntk_t *pNtk,
                        const std::vector<detail::RelocationStep> &steps)
{
    for (auto it = steps.rbegin(); it != steps.rend(); ++it)
    {
        Abc_Obj_t *pNode = Abc_NtkObj(pNtk, it->node_id);
        if (pNode)
            Abc_ObjSetPartId(pNode, it->from);
    }
}

bool ApplyRelocationLog(Abc_Ntk_t *pNtk,
                        const std::vector<detail::RelocationStep> &steps,
                        std::vector<detail::RelocationStep> &applied)
{
    applied.clear();
    for (const auto &step : steps)
    {
        Abc_Obj_t *pNode = Abc_NtkObj(pNtk, step.node_id);
        if (!pNode || !Abc_ObjIsNode(pNode)
            || Abc_ObjGetPartId(pNode) != step.from
            || step.to == ABC_PART_ID_NONE || step.to == step.from)
        {
            RollbackRelocation(pNtk, applied);
            applied.clear();
            return false;
        }
        Abc_ObjSetPartId(pNode, step.to);
        applied.push_back(step);
    }
    return true;
}

bool RelocationLimitsHold(Abc_Ntk_t *pNtk,
                          const detail::OptimizationState &state,
                          const detail::Metrics &metrics)
{
    if (metrics.hop > state.limits.hop_limit
        || metrics.nodes > state.limits.node_limit
        || metrics.cut_nets > state.limits.cutnet_limit
        || state.growth.used() > state.limits.growth_budget)
        return false;

    std::vector<int> part_sizes;
    fox::cpr::partition_sizes(pNtk, state.limits.num_parts, part_sizes);
    const int max_allowed = fox::cpr::compute_balance_max_allowed(
        part_sizes, state.limits.balance_pct);
    return fox::cpr::compute_balance_overflow(part_sizes, max_allowed)
        <= state.limits.balance_overflow_limit;
}

bool RelocationSequenceLess(const detail::RelocationSequence &lhs,
                            const detail::RelocationSequence &rhs)
{
    if (lhs.cutedge_delta != rhs.cutedge_delta)
        return lhs.cutedge_delta < rhs.cutedge_delta;
    if (lhs.steps.size() != rhs.steps.size())
        return lhs.steps.size() < rhs.steps.size();
    for (size_t i = 0; i < lhs.steps.size(); ++i)
        if (lhs.steps[i].node_id != rhs.steps[i].node_id)
            return lhs.steps[i].node_id < rhs.steps[i].node_id;
    for (size_t i = 0; i < lhs.steps.size(); ++i)
        if (lhs.steps[i].to != rhs.steps[i].to)
            return lhs.steps[i].to < rhs.steps[i].to;
    return false;
}

bool SameRelocationSequence(const detail::RelocationSequence &lhs,
                            const detail::RelocationSequence &rhs)
{
    if (lhs.steps.size() != rhs.steps.size())
        return false;
    for (size_t i = 0; i < lhs.steps.size(); ++i)
        if (lhs.steps[i].node_id != rhs.steps[i].node_id
            || lhs.steps[i].from != rhs.steps[i].from
            || lhs.steps[i].to != rhs.steps[i].to)
            return false;
    return true;
}

void CollectRelocationCandidates(Abc_Ntk_t *pNtk,
                                 const detail::OptimizationState &state,
                                 detail::TrajectoryPolicy policy,
                                 std::vector<RelocationCandidate> &candidates)
{
    std::vector<int> part_sizes;
    fox::cpr::partition_sizes(pNtk, state.limits.num_parts, part_sizes);
    candidates.clear();

    Abc_Obj_t *pNode, *pNeighbor;
    int i, k;
    Abc_NtkForEachNode(pNtk, pNode, i)
    {
        const part_id from = Abc_ObjGetPartId(pNode);
        if (from == ABC_PART_ID_NONE)
            continue;

        std::vector<part_id> targets;
        Abc_ObjForEachFanin(pNode, pNeighbor, k)
        {
            const part_id target = Abc_ObjGetPartId(pNeighbor);
            if (target != ABC_PART_ID_NONE && target != from)
                targets.push_back(target);
        }
        Abc_ObjForEachFanout(pNode, pNeighbor, k)
        {
            if (!Abc_ObjIsNode(pNeighbor))
                continue;
            const part_id target = Abc_ObjGetPartId(pNeighbor);
            if (target != ABC_PART_ID_NONE && target != from)
                targets.push_back(target);
        }
        std::sort(targets.begin(), targets.end());
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());

        const int current_cross = node_incident_cross(pNode, from);
        for (part_id target : targets)
        {
            const size_t target_index = static_cast<size_t>(target);
            const int target_size = target_index < part_sizes.size()
                ? part_sizes[target_index] : 0;
            candidates.push_back({
                {pNode->Id, from, target},
                node_incident_cross(pNode, target) - current_cross,
                target_size,
            });
        }
    }

    std::sort(candidates.begin(), candidates.end(), [policy](const auto &lhs,
                                                              const auto &rhs) {
        if (policy == detail::TrajectoryPolicy::BoundaryConcentration)
            return std::tuple{lhs.local_delta, -lhs.target_size,
                              lhs.step.node_id, lhs.step.to}
                 < std::tuple{rhs.local_delta, -rhs.target_size,
                              rhs.step.node_id, rhs.step.to};
        if (policy == detail::TrajectoryPolicy::ScarcityFirst)
            return std::tuple{lhs.target_size, lhs.local_delta,
                              lhs.step.node_id, lhs.step.to}
                 < std::tuple{rhs.target_size, rhs.local_delta,
                              rhs.step.node_id, rhs.step.to};
        return std::tuple{lhs.local_delta, lhs.step.node_id, lhs.step.to}
             < std::tuple{rhs.local_delta, rhs.step.node_id, rhs.step.to};
    });
    if (candidates.size() > kRelocationSeedLimit)
        candidates.resize(kRelocationSeedLimit);
}

} // namespace

detail::RelocationSequence detail::FindBestRelocationSequence(
    Abc_Ntk_t *pNtk, detail::OptimizationState &state,
    detail::TrajectoryPolicy policy)
{
    detail::RelocationSequence best;
    if (!pNtk || state.pNtk != pNtk)
        return best;

    const detail::Metrics baseline = detail::ComputeMetrics(pNtk);
    if (!RelocationLimitsHold(pNtk, state, baseline))
        return best;

    std::vector<detail::RelocationSequence> beam(1);
    for (int depth = 0; depth < kRelocationMaxDepth; ++depth)
    {
        std::vector<detail::RelocationSequence> next;
        for (const auto &parent : beam)
        {
            std::vector<detail::RelocationStep> parent_log;
            if (!ApplyRelocationLog(pNtk, parent.steps, parent_log))
                continue;

            std::vector<RelocationCandidate> candidates;
            CollectRelocationCandidates(pNtk, state, policy, candidates);
            for (const auto &candidate : candidates)
            {
                const bool repeated = std::any_of(
                    parent.steps.begin(), parent.steps.end(),
                    [&](const auto &step) {
                        return step.node_id == candidate.step.node_id;
                    });
                if (repeated)
                    continue;

                Abc_Obj_t *pNode = Abc_NtkObj(pNtk, candidate.step.node_id);
                if (!pNode || Abc_ObjGetPartId(pNode) != candidate.step.from)
                    continue;
                Abc_ObjSetPartId(pNode, candidate.step.to);
                const detail::Metrics metrics = detail::ComputeMetrics(pNtk);
                if (RelocationLimitsHold(pNtk, state, metrics))
                {
                    detail::RelocationSequence child = parent;
                    child.steps.push_back(candidate.step);
                    child.cutedge_delta = metrics.cut_edges - baseline.cut_edges;
                    next.push_back(std::move(child));
                }
                Abc_ObjSetPartId(pNode, candidate.step.from);
            }
            RollbackRelocation(pNtk, parent_log);
        }

        std::sort(next.begin(), next.end(), RelocationSequenceLess);
        next.erase(std::unique(next.begin(), next.end(), SameRelocationSequence),
                   next.end());
        if (next.size() > kRelocationBeamWidth)
            next.resize(kRelocationBeamWidth);
        if (next.empty())
            break;

        for (const auto &candidate : next)
            if (candidate.cutedge_delta < 0
                && (best.steps.empty() || RelocationSequenceLess(candidate, best)))
                best = candidate;
        beam = std::move(next);
    }
    return best;
}

bool detail::ApplyRelocationSequence(Abc_Ntk_t *pNtk,
                                     detail::OptimizationState &state,
                                     const detail::RelocationSequence &sequence)
{
    if (!pNtk || state.pNtk != pNtk || sequence.steps.empty())
        return false;

    const detail::Metrics before = detail::ComputeMetrics(pNtk);
    std::vector<detail::RelocationStep> applied;
    if (!ApplyRelocationLog(pNtk, sequence.steps, applied))
        return false;

    const detail::Metrics after = detail::ComputeMetrics(pNtk);
    const int actual_delta = after.cut_edges - before.cut_edges;
    if (actual_delta >= 0 || actual_delta != sequence.cutedge_delta
        || !RelocationLimitsHold(pNtk, state, after))
    {
        RollbackRelocation(pNtk, applied);
        state.AttachNetwork(pNtk);
        return false;
    }

    state.AttachNetwork(pNtk);
    if (state.Audit())
        return true;

    RollbackRelocation(pNtk, applied);
    state.AttachNetwork(pNtk);
    return false;
}

// Enumerate unordered adjacent cross-partition node pairs. Both endpoints are
// internal NODEs (Phase 0 never relocates PI/CONST1) living in different
// partitions, connected by at least one fanin/fanout edge. Each unordered pair
// appears once, deduped by (min Id, max Id). These are the only pairs where a
// swap changes any edge *between* the two -- a non-adjacent swap equals two
// independent single-node moves already handled by run_phase0_relocate.
static void collect_swap_candidates(Abc_Ntk_t *pNtk,
                                    std::vector<std::pair<int, int>> &pairs)
{
    pairs.clear();
    Abc_Obj_t *pObj, *pNb;
    int i, k;
    Abc_NtkForEachNode(pNtk, pObj, i)
    {
        part_id pa = Abc_ObjGetPartId(pObj);
        if (pa == ABC_PART_ID_NONE)
            continue;
        auto consider = [&](Abc_Obj_t *nb) {
            if (!nb || !Abc_ObjIsNode(nb))
                return;
            part_id pb = Abc_ObjGetPartId(nb);
            if (pb == ABC_PART_ID_NONE || pb == pa)
                return;
            int lo = pObj->Id < nb->Id ? pObj->Id : nb->Id;
            int hi = pObj->Id < nb->Id ? nb->Id : pObj->Id;
            pairs.emplace_back(lo, hi);
        };
        Abc_ObjForEachFanin(pObj, pNb, k)
            consider(pNb);
        Abc_ObjForEachFanout(pObj, pNb, k)
            consider(pNb);
    }
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
}

// ---------------------------------------------------------------------
// Phase 0 (swap): exchange the partitions of two adjacent cross-partition
// nodes when the swap strictly lowers global cut-edges without worsening
// hop. Runs after single-node relocation converges. Swaps preserve every
// partition's size (each nets -1+1=0), so no balance gate is needed. Pure
// part_id relabel (zero area, zero logic). The cut-edge delta is a full
// ComputeCutEdgeCount front/back diff (matches Phase 2's accept check) --
// this sidesteps double-counting the A-B edge that a hand-written local
// delta would have to special-case.
// ---------------------------------------------------------------------
static void run_phase0_swap(Abc_Ntk_t *pNtk, const Config &cfg,
                            detail::OptimizationState &state, int &total_swaps)
{
    const int baseline_hop = state.limits.hop_limit;
    int best_cutedges = ComputeCutEdgeCount(pNtk);
    int stall_count = 0;

    for (int round = 0; round < cfg.max_rounds; ++round)
    {
        std::vector<std::pair<int, int>> pairs;
        collect_swap_candidates(pNtk, pairs);
        if (pairs.empty())
            break;

        int round_swapped = 0;
        for (const auto &pr : pairs)
        {
            Abc_Obj_t *pA = Abc_NtkObj(pNtk, pr.first);
            Abc_Obj_t *pB = Abc_NtkObj(pNtk, pr.second);
            if (!pA || !pB || !Abc_ObjIsNode(pA) || !Abc_ObjIsNode(pB))
                continue;
            part_id pa = Abc_ObjGetPartId(pA);
            part_id pb = Abc_ObjGetPartId(pB);
            // Stale: an earlier swap this round already moved one endpoint,
            // so they may now share a partition (nothing to swap).
            if (pa == ABC_PART_ID_NONE || pb == ABC_PART_ID_NONE || pa == pb)
                continue;

            int cur = ComputeCutEdgeCount(pNtk);
            Abc_ObjSetPartId(pA, pb);
            Abc_ObjSetPartId(pB, pa);
            int nw = ComputeCutEdgeCount(pNtk);
            if (nw >= cur)
            {
                Abc_ObjSetPartId(pA, pa); // roll back both
                Abc_ObjSetPartId(pB, pb);
                continue;
            }
            if (Abc_NtkComputeHopNum(pNtk) > baseline_hop
                || Abc_NtkComputeCutSize(pNtk) > state.limits.cutnet_limit
                || !state.Audit())
            {
                Abc_ObjSetPartId(pA, pa); // roll back both
                Abc_ObjSetPartId(pB, pb);
                state.Audit();
                continue;
            }
            round_swapped++;
        }

        total_swaps += round_swapped;

        int new_cutedges = ComputeCutEdgeCount(pNtk);
        if (cfg.verbose)
        {
            int cur_hop = Abc_NtkComputeHopNum(pNtk);
            printf("csr: phase0 swap round %2d  swaps=%3d  cut-edges=%d  hop=%d/%d\n",
                   round, round_swapped, new_cutedges, cur_hop, baseline_hop);
        }

        if (new_cutedges < best_cutedges)
        {
            best_cutedges = new_cutedges;
            stall_count = 0;
        }
        else if (++stall_count >= cfg.stall_limit)
        {
            break;
        }

        if (round_swapped == 0)
            break;
    }
}

// ---------------------------------------------------------------------
// Phase 0: hop-preserving node relocation. Greedily moves each node to the
// neighbor partition minimizing its incident cut-edges. Pure part_id
// relabel (zero area, zero logic). Gated by strict cut-edge decrease, an
// exact global-hop-non-worsening check, and a per-partition balance cap.
// ---------------------------------------------------------------------
static void run_phase0_relocate(Abc_Ntk_t *pNtk, const Config &cfg,
                                detail::OptimizationState &state,
                                int &total_moves, int &total_swaps,
                                int &total_compound)
{
    std::vector<int> sz = state.part_sizes;
    const int max_allowed = fox::cpr::compute_balance_max_allowed(sz, state.limits.balance_pct);

    const int baseline_hop = state.limits.hop_limit;
    int best_cutedges = ComputeCutEdgeCount(pNtk);
    int stall_count = 0;

    struct Move {
        int node_id;
        part_id target;
        int delta;
    };

    for (int round = 0; round < cfg.max_rounds; ++round)
    {
        std::vector<Move> moves;
        Abc_Obj_t *pObj;
        int i;
        Abc_NtkForEachNode(pNtk, pObj, i)
        {
            if (Abc_ObjGetPartId(pObj) == ABC_PART_ID_NONE)
                continue;
            int delta;
            part_id tgt = best_relocate_target(pObj, delta);
            if (delta < 0)
                moves.push_back({pObj->Id, tgt, delta});
        }
        if (moves.empty())
            break;

        std::sort(moves.begin(), moves.end(),
                  [](const Move &a, const Move &b) { return a.delta < b.delta; });

        int round_moved = 0;
        for (const auto &mv : moves)
        {
            Abc_Obj_t *pNode = Abc_NtkObj(pNtk, mv.node_id);
            if (!pNode || !Abc_ObjIsNode(pNode))
                continue;
            part_id cur = Abc_ObjGetPartId(pNode);
            if (cur == ABC_PART_ID_NONE)
                continue;

            // Re-verify best target against current (possibly changed)
            // neighbor partitions -- earlier moves this round may have made
            // the precomputed delta stale.
            int delta;
            part_id tgt = best_relocate_target(pNode, delta);
            if (delta >= 0 || tgt == cur)
                continue;

            // Balance cap.
            if (sz[tgt] + 1 > max_allowed)
                continue;

            // Apply tentatively, then exact global hop check.
            Abc_ObjSetPartId(pNode, tgt);
            int new_hop = Abc_NtkComputeHopNum(pNtk);
            if (new_hop > baseline_hop
                || Abc_NtkComputeCutSize(pNtk) > state.limits.cutnet_limit)
            {
                Abc_ObjSetPartId(pNode, cur); // roll back
                continue;
            }

            sz[cur] -= 1;
            sz[tgt] += 1;
            if (!state.Audit())
            {
                Abc_ObjSetPartId(pNode, cur);
                sz[cur] += 1;
                sz[tgt] -= 1;
                state.Audit();
                continue;
            }
            round_moved++;
        }

        total_moves += round_moved;

        int new_cutedges = ComputeCutEdgeCount(pNtk);
        if (cfg.verbose)
        {
            int cur_hop = Abc_NtkComputeHopNum(pNtk);
            printf("csr: phase0 round %2d  moves=%3d  cut-edges=%d  hop=%d/%d\n",
                   round, round_moved, new_cutedges, cur_hop, baseline_hop);
        }

        if (new_cutedges < best_cutedges)
        {
            best_cutedges = new_cutedges;
            stall_count = 0;
        }
        else if (++stall_count >= cfg.stall_limit)
        {
            break;
        }

        if (round_moved == 0)
            break;
    }

    run_phase0_swap(pNtk, cfg, state, total_swaps);

    const auto policy = static_cast<detail::TrajectoryPolicy>(
        std::min(state.trajectory_id, 2));
    for (int round = 0; round < cfg.max_rounds; ++round)
    {
        const auto sequence = detail::FindBestRelocationSequence(pNtk, state, policy);
        if (sequence.steps.empty()
            || !detail::ApplyRelocationSequence(pNtk, state, sequence))
            break;
        total_compound++;
    }
    if (cfg.verbose)
        printf("csr: phase0 compound=%d\n", total_compound);
}

static bool run_balance_repair(Abc_Ntk_t *&pNtk, const Config &cfg,
                               detail::OptimizationState &state)
{
    Abc_Ntk_t *pRepair = Abc_NtkDup(pNtk);
    if (!pRepair)
        return false;

    detail::RestorePdbMetadata(pRepair, state.limits);
    const int cut_edges_before = ComputeCutEdgeCount(pNtk);
    std::vector<float> zero_arrival(Abc_NtkObjNumMax(pRepair), 0.0f);
    const bool repaired = fox::cpr::enforce_balance(
        pRepair, state.limits.num_parts, state.limits.balance_pct,
        zero_arrival, cfg.verbose);

    state.AttachNetwork(pRepair);
    const bool accept = repaired && ComputeCutEdgeCount(pRepair) <= cut_edges_before
        && state.Audit();
    if (accept)
    {
        Abc_NtkDelete(pNtk);
        pNtk = pRepair;
        return true;
    }

    Abc_NtkDelete(pRepair);
    state.AttachNetwork(pNtk);
    return false;
}

static bool optimize_trajectory(Abc_Ntk_t *&pNtk, const Config &cfg,
                                detail::OptimizationState &state,
                                int trajectory_id)
{
    if (!state.Audit())
    {
        return false;
    }

    const int initial_cutedges = state.entry.cut_edges;
    if (cfg.verbose)
        printf("csr: trajectory %d initial cut-edges = %d\n", trajectory_id, initial_cutedges);

    int total_moves = 0, total_swaps = 0, total_compound = 0;
    if (cfg.do_relocate)
        run_phase0_relocate(pNtk, cfg, state, total_moves, total_swaps,
                            total_compound);
    int after_phase0 = ComputeCutEdgeCount(pNtk);

    int total_attempts = 0, total_successes = 0;
    detail::Phase1Stats plan_stats;
    detail::RunPhase1Resub(pNtk, state, cfg, plan_stats);
    total_attempts += plan_stats.attempts;
    total_successes += plan_stats.successes;
    run_phase1_resub(pNtk, cfg, state, total_attempts, total_successes);
    int after_phase1 = ComputeCutEdgeCount(pNtk);

    int total_replications = 0;
    run_phase2_replicate(pNtk, cfg, state, total_replications);
    int after_phase2 = ComputeCutEdgeCount(pNtk);

    if (cfg.do_balance_repair)
        run_balance_repair(pNtk, cfg, state);

    int final_cutedges = ComputeCutEdgeCount(pNtk);

    printf("csr: cut-edges %d -> %d (after phase0=%d, after phase1=%d, after phase2=%d)\n",
           initial_cutedges, final_cutedges, after_phase0, after_phase1, after_phase2);
    printf("csr: phase0 %d moves / %d swaps / %d compound; phase1 %d attempts / %d successes; phase2 %d replications\n",
           total_moves, total_swaps, total_compound, total_attempts,
           total_successes, total_replications);
    if (cfg.verbose)
        printf("csr: phase1 plans single-remove=%d joint-remove=%d joint-replace=%d multi-divisor=%d\n",
               plan_stats.single_removals, plan_stats.joint_removals,
               plan_stats.joint_replacements, plan_stats.multi_divisor);

    return state.Audit();
}

detail::TrajectoryResult detail::TakeBestTrajectory(
    std::vector<detail::TrajectoryResult> &results,
    const detail::EntryLimits &limits, detail::NetworkDeleteFn delete_fn)
{
    size_t winner = results.size();
    for (size_t i = 0; i < results.size(); ++i)
    {
        if (!results[i].valid || !results[i].pNtk)
            continue;
        if (winner == results.size() || detail::BetterResult(results[i], results[winner]))
            winner = i;
    }

    detail::TrajectoryResult selected;
    for (size_t i = 0; i < results.size(); ++i)
    {
        if (i == winner)
        {
            selected = results[i];
            detail::RestorePdbMetadata(selected.pNtk, limits);
        }
        else if (results[i].pNtk)
        {
            delete_fn(results[i].pNtk);
        }
        results[i].pNtk = nullptr;
    }
    results.clear();
    return selected;
}

bool ApplyCsr(Abc_Frame_t *pAbc, const Config &cfg)
{
    Abc_Ntk_t *pEntry = pAbc ? Abc_FrameReadNtk(pAbc) : nullptr;
    if (!pEntry)
    {
        printf("csr: current network is empty\n");
        return false;
    }
    if (!Abc_NtkIsLogic(pEntry))
    {
        printf("csr: network must be logic (not AIG)\n");
        return false;
    }
    if (!pEntry->pPdb)
    {
        printf("csr: no partition database (run hpart first)\n");
        return false;
    }

    const detail::EntryLimits limits = detail::CaptureEntryLimits(pEntry, cfg);
    std::vector<detail::TrajectoryResult> results;
    for (int trajectory_id = 0; trajectory_id < cfg.num_trajectories; ++trajectory_id)
    {
        const auto start = std::chrono::steady_clock::now();
        Abc_Ntk_t *pTrajectory = Abc_NtkDup(pEntry);
        if (!pTrajectory)
            continue;
        detail::OptimizationState state(pTrajectory, limits, trajectory_id);
        const bool valid = optimize_trajectory(pTrajectory, cfg, state, trajectory_id);
        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        printf("csr: trajectory %d cut-edge=%d cut-net=%d hop=%d nodes=%d sec=%.2f valid=%d\n",
               trajectory_id, state.current.cut_edges, state.current.cut_nets,
               state.current.hop, state.current.nodes, seconds, valid);
        if (valid)
            results.push_back({pTrajectory, state.current, trajectory_id, true});
        else
            Abc_NtkDelete(pTrajectory);
    }

    if (results.empty())
        return false;

    auto selected = detail::TakeBestTrajectory(results, limits, Abc_NtkDelete);
    if (!selected.pNtk)
        return false;
    printf("csr: selected trajectory %d\n", selected.trajectory_id);
    Abc_FrameReplaceCurrentNetwork(pAbc, selected.pNtk);
    return true;
}

} // namespace fox::csr
