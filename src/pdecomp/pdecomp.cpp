#include "pdecomp.hpp"

#include "base/abc/abc.h"
#include "base/abc/abcPdb.hpp"

#include <cstdio>
#include <vector>

extern "C" {
#include "bool/kit/kit.h"

// Declaration is commented out in kit.h; forward-declare per csr.cpp precedent.
Hop_Obj_t *Kit_TruthToHop(Hop_Man_t *pMan, unsigned *pTruth,
                            int nVars, Vec_Int_t *vMemory);
unsigned *Hop_ManConvertAigToTruth(Hop_Man_t *p, Hop_Obj_t *pRoot,
                                    int nVars, Vec_Int_t *vTruth, int fMsbFirst);
}

namespace fox::pdecomp {

constexpr int PDECOMP_FANIN_MAX = 32;

// Cofactor recursion adapted from csr.cpp's shannon_decompose_csr. Every new
// node inherits targetPartId directly -- no partition selection -- because
// decomposition only reorganizes logic inside a single LUT using the same
// fanin set, so it must not introduce any new cross-partition edges.
static Abc_Obj_t *decompose_node(Abc_Ntk_t *pNtk, unsigned *pTruth, int nVars,
                                  Abc_Obj_t **fanins, part_id targetPartId, int K)
{
    Hop_Man_t *pHop = (Hop_Man_t *)pNtk->pManFunc;

    if (nVars <= K)
    {
        Abc_Obj_t *pLeaf = Abc_NtkCreateNode(pNtk);
        for (int v = 0; v < nVars; v++)
            Abc_ObjAddFanin(pLeaf, fanins[v]);
        pLeaf->pData = (void *)Kit_TruthToHop(pHop, pTruth, nVars, NULL);
        if (!pLeaf->pData)
            pLeaf->pData = (void *)Hop_ManConst0(pHop);
        Abc_ObjSetPartId(pLeaf, targetPartId);
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

    Abc_Obj_t *subFanins[PDECOMP_FANIN_MAX];
    int k = 0;
    for (int v = 0; v < nVars; v++)
        if (v != iVar) { subFanins[k] = fanins[v]; k++; }

    Abc_Obj_t *pN0 = decompose_node(pNtk, cof0.data(), nVars - 1, subFanins, targetPartId, K);
    Abc_Obj_t *pN1 = decompose_node(pNtk, cof1.data(), nVars - 1, subFanins, targetPartId, K);

    Abc_Obj_t *pMux = Abc_NtkCreateNode(pNtk);
    Abc_ObjAddFanin(pMux, fanins[iVar]);
    Abc_ObjAddFanin(pMux, pN1);
    Abc_ObjAddFanin(pMux, pN0);
    pMux->pData = (void *)Hop_Mux(pHop, Hop_IthVar(pHop, 0),
                                   Hop_IthVar(pHop, 1), Hop_IthVar(pHop, 2));
    Abc_ObjSetPartId(pMux, targetPartId);
    return pMux;
}

// Decomposes a single >K-input node in place, splicing the decomposition
// tree's root in as a replacement. No-op if the node is already <= K inputs.
static bool decompose_one(Abc_Ntk_t *pNtk, Abc_Obj_t *pNode, int K)
{
    int nFanins = Abc_ObjFaninNum(pNode);
    if (nFanins <= K)
        return true;
    if (nFanins > PDECOMP_FANIN_MAX)
        return false;

    Hop_Man_t *pHopMan = (Hop_Man_t *)pNtk->pManFunc;
    Hop_Obj_t *pFunc = (Hop_Obj_t *)pNode->pData;
    Vec_Int_t *vTruth = Vec_IntAlloc(Kit_TruthWordNum(nFanins) * 2 + 32);
    unsigned *pTruth = Hop_ManConvertAigToTruth(pHopMan, pFunc, nFanins, vTruth, 0);
    if (!pTruth)
    {
        Vec_IntFree(vTruth);
        return false;
    }

    std::vector<unsigned> ttCopy(pTruth, pTruth + Kit_TruthWordNum(nFanins));
    Vec_IntFree(vTruth);

    Abc_Obj_t *fanins[PDECOMP_FANIN_MAX];
    Abc_Obj_t *pFanin;
    int i = 0;
    Abc_ObjForEachFanin(pNode, pFanin, i)
        fanins[i] = pFanin;

    part_id partId = Abc_ObjGetPartId(pNode);

    // A naive cofactor tree has no subgraph sharing: a variable cofactored
    // out late is referenced by every one of the 2^depth branches still
    // live at that depth. If that variable is a cross-partition fanin, its
    // single original cut-edge would multiply into many. Route each
    // cross-partition fanin through a same-partition 1-input identity
    // buffer first, so the decomposition tree's internal repeated
    // references all land on the buffer (same partition, no new cut-edges)
    // while the one real cross-partition edge (original fanin -> buffer)
    // stays exactly as it was.
    for (int v = 0; v < nFanins; v++)
    {
        part_id faninPart = Abc_ObjGetPartId(fanins[v]);
        if (faninPart == ABC_PART_ID_NONE || faninPart == partId)
            continue;
        Abc_Obj_t *pBuf = Abc_NtkCreateNode(pNtk);
        Abc_ObjAddFanin(pBuf, fanins[v]);
        pBuf->pData = (void *)Hop_IthVar(pHopMan, 0);
        Abc_ObjSetPartId(pBuf, partId);
        fanins[v] = pBuf;
    }

    Abc_Obj_t *pRoot = decompose_node(pNtk, ttCopy.data(), nFanins, fanins, partId, K);
    Abc_ObjTransferFanout(pNode, pRoot);
    Abc_NtkDeleteObj_rec(pNode, 1);
    return true;
}

bool ApplyPdecomp(Abc_Frame_t *pAbc, const Config &cfg)
{
    Abc_Ntk_t *pNtk = Abc_FrameReadNtk(pAbc);
    if (!pNtk)
    {
        printf("pdecomp: network is null\n");
        return false;
    }
    if (!Abc_NtkIsLogic(pNtk))
    {
        printf("pdecomp: network must be logic (not AIG)\n");
        return false;
    }
    if (!pNtk->pPdb)
    {
        printf("pdecomp: no partition database (run hpart first)\n");
        return false;
    }

    int initial_hop = Abc_NtkComputeHopNum(pNtk);
    int initial_cutedge = Abc_NtkComputeCutEdgeNum(pNtk);

    // Snapshot for whole-network rollback. Abc_NtkDup preserves every
    // object's part_id via Abc_NtkDupObj -> Abc_ObjSetPartId, so this is a
    // complete, correctly-partitioned restore target if the invariant check
    // below fails.
    Abc_Ntk_t *pSnapshot = Abc_NtkDup(pNtk);

    std::vector<int> targetIds;
    Abc_Obj_t *pObj;
    int i;
    Abc_NtkForEachNode(pNtk, pObj, i)
        if (Abc_ObjFaninNum(pObj) > cfg.K)
            targetIds.push_back(pObj->Id);

    bool ok = true;
    for (int id : targetIds)
    {
        Abc_Obj_t *pNode = Abc_NtkObj(pNtk, id);
        if (!pNode || !Abc_ObjIsNode(pNode))
            continue; // deleted as a byproduct of an earlier decomposition
        if (!decompose_one(pNtk, pNode, cfg.K))
        {
            ok = false;
            break;
        }
    }

    if (ok)
    {
        // Abc_NtkSweep is deliberately not used here: it converts the
        // network to BDD and actively patches out any <2-fanin node
        // (Abc_ObjPatchFanin), which would delete the cross-partition
        // identity buffers inserted above and reopen the fanout-explosion
        // bug they exist to prevent. Abc_NtkCleanup only drops nodes
        // unreachable from the POs, leaving the buffers (which do have live
        // fanout) untouched.
        Abc_NtkCleanup(pNtk, 0);
        int final_hop = Abc_NtkComputeHopNum(pNtk);
        int final_cutedge = Abc_NtkComputeCutEdgeNum(pNtk);
        if (final_hop != initial_hop || final_cutedge != initial_cutedge)
        {
            printf("pdecomp: partition invariant violated (hop %d->%d, cut-edge %d->%d), aborting\n",
                   initial_hop, final_hop, initial_cutedge, final_cutedge);
            ok = false;
        }
    }

    if (!ok)
    {
        // Discards the mutated (now invalid) pNtk and installs the pristine
        // snapshot as the frame's current network.
        Abc_FrameReplaceCurrentNetwork(pAbc, pSnapshot);
        return false;
    }

    Abc_NtkDelete(pSnapshot);
    printf("pdecomp: decomposed to K<=%d (hop=%d, cut-edge=%d unchanged)\n",
           cfg.K, initial_hop, initial_cutedge);
    return true;
}

} // namespace fox::pdecomp
