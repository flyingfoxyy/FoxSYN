#include "csr.hpp"

#include "base/abc/abc.h"
#include "base/abc/abcPdb.hpp"
#include "cpr/cpr.hpp"

#include <algorithm>
#include <cstdio>
#include <unordered_map>
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

// ---------------------------------------------------------------------
// Candidate collection: every (consumer, iFanin) pair whose driver crosses
// into a different partition, weighted by the driver's total number of
// cross-partition fanouts. Scans the whole network (cutsize is structural,
// not path-dependent), unlike cmfs's top-K critical-path scan.
// ---------------------------------------------------------------------

struct CutCandidate {
    int node_id;
    int iFanin;
    int weight;
};

static void collect_cut_candidates(Abc_Ntk_t *pNtk, std::vector<CutCandidate> &candidates)
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
            CutCandidate c;
            c.node_id = pObj->Id;
            c.iFanin  = k;
            c.weight  = driver_cross_count[pFanin->Id];
            candidates.push_back(c);
        }
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const CutCandidate &a, const CutCandidate &b) {
                  return a.weight > b.weight;
              });
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

static int try_partition_resub(Mfs_Man_t *p, Abc_Obj_t *pNode, int iFanin,
                               int maxTempLut, int *reason)
{
    int pCands[MFS_FANIN_MAX];
    int nCands = 0;
    Abc_Obj_t *pFanin;
    int i;

    *reason = RR_RESUB_FAIL;

    part_id consumer_part = Abc_ObjGetPartId(pNode);

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

static void run_phase1_resub(Abc_Ntk_t *pNtk, const Config &cfg,
                             int &total_attempts, int &total_successes)
{
    int best_cutedges = ComputeCutEdgeCount(pNtk);
    int stall_count = 0;

    for (int round = 0; round < cfg.max_rounds; ++round)
    {
        Abc_NtkToSop(pNtk, -1, ABC_INFINITY);
        if (!Abc_NtkToAig(pNtk))
            break;

        std::vector<CutCandidate> candidates;
        collect_cut_candidates(pNtk, candidates);
        if (candidates.empty())
            break;

        Abc_NtkLevel(pNtk);
        Abc_NtkStartReverseLevels(pNtk, 0);

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

            int ret = try_partition_resub(p, pNode, cand.iFanin, cfg.maxTempLut, &reason);

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

        Abc_NtkSweep(pNtk, 0);
        Abc_NtkCleanup(pNtk, 0);

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

static bool try_replicate(Abc_Ntk_t *pNtk, Abc_Obj_t *pDriver, Abc_Obj_t *pConsumer,
                          const HopSlack &hs, int &cur_cutedges, int &extra_nodes)
{
    // Only logic nodes can be duplicated. A PI/CONST1 driver is a single
    // physical pin/constant in the design; "duplicating" it would fabricate
    // a second primary input, corrupting the network (matches cpr's
    // restriction in ordered_path_nodes/try_replicate_on_path).
    if (!Abc_ObjIsNode(pDriver))
        return false;

    part_id target_part = Abc_ObjGetPartId(pConsumer);
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
    if (new_cutedges < cur_cutedges)
    {
        cur_cutedges = new_cutedges;
        extra_nodes += 1;
        return true;
    }

    for (Abc_Obj_t *pF : fanouts)
        Abc_ObjPatchFanin(pF, pDup, pDriver);
    Abc_NtkDeleteObj(pDup);
    return false;
}

static void run_phase2_replicate(Abc_Ntk_t *pNtk, const Config &cfg,
                                 int &total_replications)
{
    const int initial_nodes = Abc_NtkNodeNum(pNtk);
    const int max_extra_nodes =
        static_cast<int>(static_cast<long long>(initial_nodes) * cfg.replicate_growth_pct / 100);
    int extra_nodes = 0;
    int cur_cutedges = ComputeCutEdgeCount(pNtk);
    int stall_count = 0;

    // Snapshot taken once, before Phase 2 starts (Phase 1 has already
    // converged, so the network is stable at this point). Later rounds
    // reuse the same snapshot -- see docs/superpowers/specs for the
    // rationale (a conservative, not dynamically-updated, hop budget).
    HopSlack hs;
    compute_hop_slack(pNtk, hs);

    for (int round = 0; round < cfg.max_rounds; ++round)
    {
        if (extra_nodes >= max_extra_nodes)
        {
            if (cfg.verbose)
                printf("csr: phase2 node budget exhausted (%d/%d), stopping\n",
                       extra_nodes, max_extra_nodes);
            break;
        }

        std::vector<CutCandidate> candidates;
        collect_cut_candidates(pNtk, candidates);
        if (candidates.empty())
            break;

        int round_fixed = 0;
        for (const auto &cand : candidates)
        {
            if (extra_nodes >= max_extra_nodes)
                break;

            Abc_Obj_t *pConsumer = Abc_NtkObj(pNtk, cand.node_id);
            if (!pConsumer || !Abc_ObjIsNode(pConsumer))
                continue;
            if (cand.iFanin >= Abc_ObjFaninNum(pConsumer))
                continue;
            Abc_Obj_t *pDriver = Abc_ObjFanin(pConsumer, cand.iFanin);
            if (Abc_ObjGetPartId(pDriver) == Abc_ObjGetPartId(pConsumer))
                continue; // already fixed by an earlier replication this round

            if (try_replicate(pNtk, pDriver, pConsumer, hs, cur_cutedges, extra_nodes))
                round_fixed++;
        }

        if (cfg.verbose)
            printf("csr: phase2 round %2d  candidates=%3zu  replicated=%3d"
                   "  cut-edges=%d  nodes=%d/%d\n",
                   round, candidates.size(), round_fixed, cur_cutedges,
                   extra_nodes, max_extra_nodes);

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

static int resolve_num_parts(Abc_Ntk_t *pNtk)
{
    int np = pNtk->pPdb->num_parts();
    if (np > 0)
        return np;

    int max_part = 0;
    int i;
    Abc_Obj_t *pObj;
    Abc_NtkForEachObj(pNtk, pObj, i)
    {
        part_id p = Abc_ObjGetPartId(pObj);
        if (p != ABC_PART_ID_NONE && p > max_part)
            max_part = p;
    }
    return max_part + 1;
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

// ---------------------------------------------------------------------
// Phase 0: hop-preserving node relocation. Greedily moves each node to the
// neighbor partition minimizing its incident cut-edges. Pure part_id
// relabel (zero area, zero logic). Gated by strict cut-edge decrease, an
// exact global-hop-non-worsening check, and a per-partition balance cap.
// ---------------------------------------------------------------------
void run_phase0_relocate(Abc_Ntk_t *pNtk, const Config &cfg, int &total_moves)
{
    int num_parts = resolve_num_parts(pNtk);
    int balance_pct = cfg.balance_pct;
    if (balance_pct < 0)
        balance_pct = pNtk->pPdb->balance_pct();
    if (balance_pct < 0)
        balance_pct = 2;

    std::vector<int> sz;
    fox::cpr::partition_sizes(pNtk, num_parts, sz);
    int max_allowed = fox::cpr::compute_balance_max_allowed(sz, balance_pct);

    const int baseline_hop = Abc_NtkComputeHopNum(pNtk);
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
            if (new_hop > baseline_hop)
            {
                Abc_ObjSetPartId(pNode, cur); // roll back
                continue;
            }

            sz[cur] -= 1;
            sz[tgt] += 1;
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
}

bool ApplyCsr(Abc_Ntk_t *pNtk, const Config &cfg)
{
    if (!pNtk)
    {
        printf("csr: network is null\n");
        return false;
    }
    if (!Abc_NtkIsLogic(pNtk))
    {
        printf("csr: network must be logic (not AIG)\n");
        return false;
    }
    if (!pNtk->pPdb)
    {
        printf("csr: no partition database (run hpart first)\n");
        return false;
    }

    int initial_cutedges = ComputeCutEdgeCount(pNtk);
    if (cfg.verbose)
        printf("csr: initial cut-edges = %d\n", initial_cutedges);

    int total_moves = 0;
    if (cfg.do_relocate)
        run_phase0_relocate(pNtk, cfg, total_moves);
    int after_phase0 = ComputeCutEdgeCount(pNtk);

    int total_attempts = 0, total_successes = 0;
    run_phase1_resub(pNtk, cfg, total_attempts, total_successes);
    int after_phase1 = ComputeCutEdgeCount(pNtk);

    int total_replications = 0;
    run_phase2_replicate(pNtk, cfg, total_replications);
    int after_phase2 = ComputeCutEdgeCount(pNtk);

    // Known limitation: enforce_balance optimizes only partition size, not
    // cutsize, so this final repair can raise cut-edges above the phase1/2
    // result (observed on adder.v: 12 -> 21). Functional correctness is
    // unaffected (cec-clean); only the cutsize objective can regress here.
    // Off by default (-b to enable) for exactly this reason.
    if (cfg.do_balance_repair)
    {
        int num_parts = resolve_num_parts(pNtk);
        int balance_pct = cfg.balance_pct;
        if (balance_pct < 0)
            balance_pct = pNtk->pPdb->balance_pct();
        if (balance_pct < 0)
            balance_pct = 2;

        // csr has no arrival/timing model; a zero vector means
        // enforce_balance's "move lowest-arrival node first" tie-break
        // degenerates to an unspecified (but still correct) choice among
        // balance-violating nodes.
        std::vector<float> zero_arrival(Abc_NtkObjNumMax(pNtk), 0.0f);
        fox::cpr::enforce_balance(pNtk, num_parts, balance_pct, zero_arrival, cfg.verbose);
    }

    int final_cutedges = ComputeCutEdgeCount(pNtk);

    printf("csr: cut-edges %d -> %d (after phase0=%d, after phase1=%d, after phase2=%d)\n",
           initial_cutedges, final_cutedges, after_phase0, after_phase1, after_phase2);
    printf("csr: phase0 %d moves; phase1 %d attempts / %d successes; phase2 %d replications\n",
           total_moves, total_attempts, total_successes, total_replications);

    return true;
}

} // namespace fox::csr
