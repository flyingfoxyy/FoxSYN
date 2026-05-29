#include "cmfs.hpp"

#include "timer/timer.hpp"
#include "base/abc/abc.h"
#include "base/abc/abcPdb.hpp"

#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <vector>

extern "C" {
#include "opt/mfs/mfsInt.h"
#include "bool/kit/kit.h"

int Abc_WinNode(Mfs_Man_t *p, Abc_Obj_t *pNode);
int Abc_NtkMfsSolveSatResub(Mfs_Man_t *p, Abc_Obj_t *pNode,
                             int iFanin, int fOnlyRemove, int fSkipUpdate);
int Abc_NtkMfsTryResubOnce(Mfs_Man_t *p, int *pCands, int nCands);
void Abc_NtkMfsUpdateNetwork(Mfs_Man_t *p, Abc_Obj_t *pObj,
                              Vec_Ptr_t *vMfsFanins, Hop_Obj_t *pFunc);
unsigned *Hop_ManConvertAigToTruth(Hop_Man_t *p, Hop_Obj_t *pRoot,
                                    int nVars, Vec_Int_t *vTruth, int fMsbFirst);
Hop_Obj_t *Kit_TruthToHop(Hop_Man_t *pMan, unsigned *pTruth,
                            int nVars, Vec_Int_t *vMemory);
}

namespace fox::cmfs {

using ::fox::timer::SimpleTimer;

static constexpr float HOP_DLY = 200.0f;

struct CandidateEdge {
    int node_id;
    int iFanin;
    float slack;
    int frequency;
    float weight;
};

static float edge_delay(Abc_Obj_t *pFrom, Abc_Obj_t *pTo, Pdb *pPdb)
{
    if (!pPdb)
        return 0.0f;
    part_id fp = pPdb->get(pFrom->Id);
    part_id tp = pPdb->get(pTo->Id);
    if (fp == ABC_PART_ID_NONE || tp == ABC_PART_ID_NONE)
        return 0.0f;
    return (fp != tp) ? HOP_DLY : 0.0f;
}

// Shannon decomposition: recursively split an over-sized truth table into
// a tree of 6-LUT nodes. Splits on highest-arrival variable (MUX select).
static Abc_Obj_t *shannon_decompose(Abc_Ntk_t *pNtk, unsigned *pTruth, int nVars,
                                     Abc_Obj_t **fanins, float *arrivals, part_id partId)
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
        Abc_ObjSetPartId(pLeaf, partId);
        return pLeaf;
    }

    // Pick highest-arrival variable as decomposition variable
    int iVar = 0;
    float maxArr = arrivals[0];
    for (int v = 1; v < nVars; v++)
        if (arrivals[v] > maxArr) { maxArr = arrivals[v]; iVar = v; }

    int nWords = Kit_TruthWordNum(nVars);
    std::vector<unsigned> cof0(nWords), cof1(nWords), tmp(nWords);
    Kit_TruthCofactor0New(cof0.data(), pTruth, nVars, iVar);
    Kit_TruthCofactor1New(cof1.data(), pTruth, nVars, iVar);

    // Compact: swap iVar to last position, then use nVars-1
    for (int v = iVar; v < nVars - 1; v++)
    {
        Kit_TruthSwapAdjacentVars(tmp.data(), cof0.data(), nVars, v);
        std::copy(tmp.begin(), tmp.end(), cof0.begin());
        Kit_TruthSwapAdjacentVars(tmp.data(), cof1.data(), nVars, v);
        std::copy(tmp.begin(), tmp.end(), cof1.begin());
    }

    // Build sub-arrays without the decomposition variable
    Abc_Obj_t *subFanins[MFS_FANIN_MAX];
    float subArrivals[MFS_FANIN_MAX];
    int k = 0;
    for (int v = 0; v < nVars; v++)
        if (v != iVar) { subFanins[k] = fanins[v]; subArrivals[k] = arrivals[v]; k++; }

    Abc_Obj_t *pN0 = shannon_decompose(pNtk, cof0.data(), nVars - 1, subFanins, subArrivals, partId);
    Abc_Obj_t *pN1 = shannon_decompose(pNtk, cof1.data(), nVars - 1, subFanins, subArrivals, partId);

    // MUX node: fanins[iVar] ? pN1 : pN0
    Abc_Obj_t *pMux = Abc_NtkCreateNode(pNtk);
    Abc_ObjAddFanin(pMux, fanins[iVar]);
    Abc_ObjAddFanin(pMux, pN1);
    Abc_ObjAddFanin(pMux, pN0);
    pMux->pData = (void *)Hop_Mux(pHop, Hop_IthVar(pHop, 0),
                                   Hop_IthVar(pHop, 1), Hop_IthVar(pHop, 2));
    Abc_ObjSetPartId(pMux, partId);
    return pMux;
}

// Arrival-aware resub: try to replace a critical fanin with any divisor
// whose arrival contribution is strictly lower. This guarantees timing gain
// at this node even if the divisor is in a different partition.
// Requires: Abc_WinNode(p, pNode) already called successfully.
static int try_arrival_resub(Mfs_Man_t *p, Abc_Obj_t *pNode, int iFanin,
                             float crit_contrib,
                             const std::vector<float> &arrival, Pdb *pPdb,
                             int maxTempLut)
{
    int pCands[MFS_FANIN_MAX];
    int nCands = 0;
    Abc_Obj_t *pFanin;
    int i;

    // Build base candidate set: all fanins except the target
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

    // First try pure removal
    Vec_PtrFillSimInfo(p->vDivCexes, 0, p->nDivWords);
    p->nCexes = 0;

    int ret = Abc_NtkMfsTryResubOnce(p, pCands, nCands);
    if (ret == 1)
    {
        Hop_Obj_t *pFunc = Abc_NtkMfsInterplate(p, pCands, nCands);
        if (!pFunc)
            return 0;
        Abc_NtkMfsUpdateNetwork(p, pNode, p->vMfsFanins, pFunc);
        p->nRemoves++;
        return 1;
    }
    if (ret == -1)
        return 0;

    // Pure removal failed. Try divisors with lower arrival contribution.
    // Pre-compute which divisors are "good" (lower contribution than critical fanin)
    std::vector<std::pair<float, int>> good_divs;
    int arr_sz = static_cast<int>(arrival.size());
    for (int d = 0; d < nDivs; d++)
    {
        Abc_Obj_t *pDiv = (Abc_Obj_t *)Vec_PtrEntry(p->vDivs, d);
        if (pDiv->Id >= arr_sz)
            continue;
        float div_contrib = arrival[pDiv->Id] + edge_delay(pDiv, pNode, pPdb);
        if (div_contrib < crit_contrib - 0.5f)
            good_divs.push_back({div_contrib, d});
    }
    // Sort ascending: prefer lowest-arrival divisors first
    std::sort(good_divs.begin(), good_divs.end());

    if (good_divs.empty())
        return 0;

    int nWords;
    unsigned *pData;
    int w;

    for (int iter = 0; iter < p->pPars->nWinMax; ++iter)
    {
        nWords = Abc_BitWordNum(p->nCexes);
        if (nWords > p->nDivWords)
            break;

        // Find next good divisor consistent with counter-examples
        int found = -1;
        for (auto &[contrib, d] : good_divs)
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
            return 0;

        pCands[nCands] = Abc_Var2Lit(Vec_IntEntry(p->vProjVarsSat, found), 1);
        ret = Abc_NtkMfsTryResubOnce(p, pCands, nCands + 1);
        if (ret == 1)
        {
            Hop_Obj_t *pFunc = Abc_NtkMfsInterplate(p, pCands, nCands + 1);
            if (!pFunc)
                return 0;
            Vec_PtrPush(p->vMfsFanins, Vec_PtrEntry(p->vDivs, found));
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
    // After removing 1 fanin and adding 2, total = N+1. Need N+1 <= K (6).
    if (Abc_ObjFaninNum(pNode) > 5)
        return 0;
    if (good_divs.size() < 2)
        return 0;

    // Limit search space
    constexpr int MAX_PAIR_DIVS = 30;
    int pair_limit = std::min(static_cast<int>(good_divs.size()), MAX_PAIR_DIVS);

    for (int iter = 0; iter < p->pPars->nWinMax; ++iter)
    {
        nWords = Abc_BitWordNum(p->nCexes);
        if (nWords > p->nDivWords)
            break;

        // Find a pair (d1, d2) whose OR covers all counter-examples
        int found1 = -1, found2 = -1;
        for (int i = 1; i < pair_limit && found1 < 0; i++)
        {
            int di = good_divs[i].second;
            unsigned *pDi = (unsigned *)Vec_PtrEntry(p->vDivCexes, di);
            for (int j = 0; j < i; j++)
            {
                int dj = good_divs[j].second;
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
            return 0;

        pCands[nCands]     = Abc_Var2Lit(Vec_IntEntry(p->vProjVarsSat, found1), 1);
        pCands[nCands + 1] = Abc_Var2Lit(Vec_IntEntry(p->vProjVarsSat, found2), 1);
        ret = Abc_NtkMfsTryResubOnce(p, pCands, nCands + 2);
        if (ret == 1)
        {
            Hop_Obj_t *pFunc = Abc_NtkMfsInterplate(p, pCands, nCands + 2);
            if (!pFunc)
                return 0;
            Vec_PtrPush(p->vMfsFanins, Vec_PtrEntry(p->vDivs, found1));
            Vec_PtrPush(p->vMfsFanins, Vec_PtrEntry(p->vDivs, found2));
            Abc_NtkMfsUpdateNetwork(p, pNode, p->vMfsFanins, pFunc);
            p->nResubs++;
            return 3; // 2-divisor resub success
        }
        if (ret == -1)
            return 0;
        if (p->nCexes >= p->pPars->nWinMax)
            break;
    }

    // Phase 4: Multi-divisor resub with Shannon decomposition (-X mode)
    if (maxTempLut < 7 || nCands + 3 > maxTempLut)
        return 0;
    if (good_divs.size() < 3)
        return 0;

    int maxExtra = std::min(maxTempLut - nCands, static_cast<int>(good_divs.size()));
    maxExtra = std::min(maxExtra, MFS_FANIN_MAX - nCands);

    // Greedy: accumulate divisors one at a time, try SAT after each addition
    for (int k = 2; k < maxExtra; ++k)
    {
        nWords = Abc_BitWordNum(p->nCexes);
        if (nWords > p->nDivWords)
            break;

        // Find next divisor covering at least one uncovered CEX
        int found = -1;
        for (int gi = 0; gi < static_cast<int>(good_divs.size()); gi++)
        {
            int d = good_divs[gi].second;
            // Skip if already chosen
            bool already = false;
            for (int c = nCands; c < nCands + k; c++)
                if (pCands[c] == Abc_Var2Lit(Vec_IntEntry(p->vProjVarsSat, d), 1))
                { already = true; break; }
            if (already) continue;

            pData = (unsigned *)Vec_PtrEntry(p->vDivCexes, d);
            // Check it covers at least one bit not yet covered
            for (w = 0; w < nWords; w++)
                if (pData[w] != 0) break;
            if (w < nWords) { found = d; break; }
        }
        if (found < 0)
            return 0;

        pCands[nCands + k] = Abc_Var2Lit(Vec_IntEntry(p->vProjVarsSat, found), 1);
        ret = Abc_NtkMfsTryResubOnce(p, pCands, nCands + k + 1);
        if (ret == 1)
        {
            // Success! Interpolate and decompose.
            Hop_Obj_t *pFunc = Abc_NtkMfsInterplate(p, pCands, nCands + k + 1);
            if (!pFunc) return 0;

            // Collect all fanins for decomposition
            for (int c = 0; c <= k; c++)
                Vec_PtrPush(p->vMfsFanins, Vec_PtrEntry(p->vDivs, good_divs[c].second));
            // Actually we need the correct divisor indices. Rebuild from pCands.
            Vec_PtrClear(p->vMfsFanins);
            Abc_Obj_t *pFi; int fi;
            Abc_ObjForEachFanin(pNode, pFi, fi)
                if (fi != iFanin)
                    Vec_PtrPush(p->vMfsFanins, pFi);
            // Add all chosen divisors (from pCands[nCands..nCands+k])
            for (int c = nCands; c <= nCands + k; c++)
            {
                int lit = pCands[c];
                int var = Abc_Lit2Var(lit);
                // Find which divisor index this corresponds to
                for (int d = 0; d < Vec_IntSize(p->vProjVarsSat); d++)
                    if (Vec_IntEntry(p->vProjVarsSat, d) == var)
                    { Vec_PtrPush(p->vMfsFanins, Vec_PtrEntry(p->vDivs, d)); break; }
            }

            int nTotal = Vec_PtrSize(p->vMfsFanins);
            if (nTotal <= 6)
            {
                // Fits in one LUT, just update normally
                Abc_NtkMfsUpdateNetwork(p, pNode, p->vMfsFanins, pFunc);
                p->nResubs++;
                return 4;
            }

            // Decompose via Shannon expansion
            Hop_Man_t *pHopMan = (Hop_Man_t *)pNode->pNtk->pManFunc;
            Vec_Int_t *vTruth = Vec_IntAlloc(Kit_TruthWordNum(nTotal) * 2 + 32);
            unsigned *pTruth = Hop_ManConvertAigToTruth(pHopMan, pFunc, nTotal, vTruth, 0);
            if (!pTruth) { Vec_IntFree(vTruth); return 0; }

            // Copy truth table (buffer may be reused)
            int nTtWords = Kit_TruthWordNum(nTotal);
            std::vector<unsigned> ttCopy(pTruth, pTruth + nTtWords);
            Vec_IntFree(vTruth);

            Abc_Obj_t *decomp_fanins[MFS_FANIN_MAX];
            float decomp_arrivals[MFS_FANIN_MAX];
            int arr_sz = static_cast<int>(arrival.size());
            for (int j = 0; j < nTotal; j++)
            {
                decomp_fanins[j] = (Abc_Obj_t *)Vec_PtrEntry(p->vMfsFanins, j);
                int fid = decomp_fanins[j]->Id;
                decomp_arrivals[j] = (fid < arr_sz) ? arrival[fid] : 0.0f;
                decomp_arrivals[j] += edge_delay(decomp_fanins[j], pNode, pPdb);
            }

            part_id partId = Abc_ObjGetPartId(pNode);
            Abc_Obj_t *pRoot = shannon_decompose(pNode->pNtk, ttCopy.data(), nTotal,
                                                  decomp_fanins, decomp_arrivals, partId);
            // Replace original node with decomposed tree
            Abc_ObjTransferFanout(pNode, pRoot);
            Abc_NtkDeleteObj(pNode);
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


static void collect_candidates(Abc_Ntk_t *pNtk, const std::vector<float> &arrival,
                               const std::vector<fox::timer::Path> &paths,
                               std::vector<CandidateEdge> &candidates)
{
    Pdb *pPdb = pNtk->pPdb;
    std::unordered_map<uint64_t, CandidateEdge> edge_map;

    for (const auto &path : paths)
    {
        for (int id : path.ids)
        {
            Abc_Obj_t *pNode = Abc_NtkObj(pNtk, id);
            if (!pNode || !Abc_ObjIsNode(pNode))
                continue;
            if (Abc_ObjFaninNum(pNode) < 2)
                continue;

            float best = -1.0f, second = -1.0f;
            int best_idx = -1;
            Abc_Obj_t *pFi;
            int k;
            Abc_ObjForEachFanin(pNode, pFi, k)
            {
                float c = arrival[pFi->Id] + edge_delay(pFi, pNode, pPdb);
                if (c > best)
                {
                    second = best;
                    best = c;
                    best_idx = k;
                }
                else if (c > second)
                {
                    second = c;
                }
            }

            float slack = best - second;
            if (slack < 0.5f || best_idx < 0)
                continue;

            uint64_t key = (static_cast<uint64_t>(pNode->Id) << 16)
                         | static_cast<uint64_t>(best_idx);
            auto &entry = edge_map[key];
            entry.node_id = pNode->Id;
            entry.iFanin = best_idx;
            entry.slack = slack;
            entry.frequency += 1;
        }
    }

    candidates.clear();
    candidates.reserve(edge_map.size());
    for (auto &[key, e] : edge_map)
    {
        e.weight = e.slack * static_cast<float>(e.frequency);
        candidates.push_back(e);
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const CandidateEdge &a, const CandidateEdge &b) {
                  return a.weight > b.weight;
              });
}

bool ApplyCmfs(Abc_Ntk_t *pNtk, const Config &cfg)
{
    if (!pNtk)
    {
        printf("cmfs: network is null\n");
        return false;
    }
    if (!Abc_NtkIsLogic(pNtk))
    {
        printf("cmfs: network must be logic (not AIG)\n");
        return false;
    }
    if (!pNtk->pPdb)
    {
        printf("cmfs: no partition database (run hpart first)\n");
        return false;
    }

    // Measure true initial arrival before any transformation
    int total_attempts = 0, total_successes = 0, total_timeouts = 0;

    SimpleTimer timer0(pNtk);
    timer0.compute_arrival();
    float initial_arrival = timer0.max_arrival();

    // Force a clean Hop manager: convert to SOP first, then back to AIG.
    // Note: Abc_NtkToSop calls Abc_NtkMinimumBase which removes locally-redundant
    // fanins. This is a beneficial side effect that we account for in timing.
    Abc_NtkToSop(pNtk, -1, ABC_INFINITY);
    if (!Abc_NtkToAig(pNtk))
    {
        printf("cmfs: failed to convert to AIG representation\n");
        return false;
    }

    SimpleTimer timer(pNtk);
    timer.compute_arrival();
    float post_minbase_arrival = timer.max_arrival();
    float best_arrival = post_minbase_arrival;
    int stall_count = 0;

    if (cfg.verbose)
    {
        printf("cmfs: initial max arrival = %.2f\n", initial_arrival);
        if (post_minbase_arrival < initial_arrival - 0.5f)
            printf("cmfs: after minbase      = %.2f (gain %.2f from redundant fanin removal)\n",
                   post_minbase_arrival, initial_arrival - post_minbase_arrival);
        printf("cmfs: top_K=%d  max_rounds=%d  stall=%d  BT=%d  resub=%s\n",
               cfg.top_K, cfg.max_rounds, cfg.stall_limit, cfg.nBTLimit,
               cfg.allow_resub ? "on" : "off");
        fflush(stdout);
    }

    int rounds_run = 0;
    for (int round = 0; round < cfg.max_rounds; ++round)
    {
        rounds_run++;
        // Re-establish clean Hop manager each round (sweep/cleanup may invalidate it)
        Abc_NtkToSop(pNtk, -1, ABC_INFINITY);
        if (!Abc_NtkToAig(pNtk))
            break;

        Abc_NtkLevel(pNtk);
        Abc_NtkStartReverseLevels(pNtk, 0);

        SimpleTimer rtimer(pNtk);
        rtimer.compute_arrival();
        auto paths = rtimer.extract_top_paths(cfg.top_K);
        if (paths.empty())
        {
            Abc_NtkStopReverseLevels(pNtk);
            break;
        }

        std::vector<CandidateEdge> candidates;
        collect_candidates(pNtk, rtimer.get_arrival(), paths, candidates);

        if (candidates.empty())
        {
            Abc_NtkStopReverseLevels(pNtk);
            break;
        }

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
        const auto &arr = rtimer.get_arrival();
        int arr_size = static_cast<int>(arr.size());

        for (const auto &cand : candidates)
        {
            if (static_cast<size_t>(cand.node_id) < node_done.size()
                && node_done[cand.node_id])
                continue;

            Abc_Obj_t *pNode = Abc_NtkObj(pNtk, cand.node_id);
            if (!pNode || !Abc_ObjIsNode(pNode))
                continue;
            if (Abc_ObjFaninNum(pNode) < 2)
                continue;
            if (cand.iFanin >= Abc_ObjFaninNum(pNode))
                continue;
            if (Abc_ObjFaninNum(pNode) > p->nFaninMax)
                continue;

            if (Abc_WinNode(p, pNode) != 0)
                continue;

            part_id orig_part = Abc_ObjGetPartId(pNode);
            int id_before = Abc_NtkObjNumMax(pNtk);

            total_attempts++;
            int ret = 0;

            if (cfg.allow_resub)
            {
                Abc_Obj_t *pCritFanin = Abc_ObjFanin(pNode, cand.iFanin);
                if (pCritFanin->Id >= arr_size)
                    continue;
                float crit_contrib = arr[pCritFanin->Id]
                                   + edge_delay(pCritFanin, pNode, pNtk->pPdb);
                ret = try_arrival_resub(p, pNode, cand.iFanin,
                                        crit_contrib, arr, pNtk->pPdb,
                                        cfg.maxTempLut);
            }
            else
            {
                ret = Abc_NtkMfsSolveSatResub(p, pNode, cand.iFanin, 1, 0);
            }

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

        total_timeouts += p->nTimeOuts;
        Mfs_ManStop(p);
        Abc_NtkStopReverseLevels(pNtk);

        Abc_NtkSweep(pNtk, 0);
        Abc_NtkCleanup(pNtk, 0);

        SimpleTimer ptimer(pNtk);
        ptimer.compute_arrival();
        float new_arrival = ptimer.max_arrival();

        if (cfg.verbose)
        {
            printf("cmfs: round %2d  candidates=%3zu  removed=%3d  arrival=%.2f\n",
                   round, candidates.size(), round_successes, new_arrival);
        }

        if (new_arrival < best_arrival - 0.5f)
        {
            best_arrival = new_arrival;
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

    // Final report
    SimpleTimer ftimer(pNtk);
    ftimer.compute_arrival();
    float final_arrival = ftimer.max_arrival();

    printf("cmfs: %d rounds, %d attempts, %d successes (%d timeouts)\n",
           rounds_run, total_attempts, total_successes, total_timeouts);
    printf("cmfs: arrival %.2f -> %.2f (total improvement %.2f)\n",
           initial_arrival, final_arrival, initial_arrival - final_arrival);

    return true;
}

} // namespace fox::cmfs


