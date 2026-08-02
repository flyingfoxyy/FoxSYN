#include "csr4/csr4.hpp"
#include "csr4/csr4_internal.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "base/abc/abc.h"
#include "base/abc/abcPdb.hpp"

extern "C" {
#include "bool/kit/kit.h"

// Declared in aig/hop/hop.h:329, but that header is not safe to include from
// C++ here; forward-declare it, matching src/pdecomp/pdecomp.cpp:15-16.
unsigned *Hop_ManConvertAigToTruth(Hop_Man_t *p, Hop_Obj_t *pRoot,
                                    int nVars, Vec_Int_t *vTruth, int fMsbFirst);
}

namespace fox::csr4 {

int ceil_log2(long m)
{
    if (m <= 1)
        return 0;
    int bits = 0;
    long v = m - 1;
    while (v > 0) { v >>= 1; ++bits; }
    return bits;
}

bool node_truth_u64(Abc_Obj_t *pNode, uint64_t &out)
{
    int nFanins = Abc_ObjFaninNum(pNode);
    if (nFanins < 1 || nFanins > CSR4_MAX_FANIN)
        return false;

    Abc_Ntk_t *pNtk = pNode->pNtk;
    if (Abc_NtkHasSop(pNtk))
    {
        out = Abc_SopToTruth((char *)pNode->pData, nFanins);
        return true;
    }
    if (!Abc_NtkHasAig(pNtk))
        return false;

    Hop_Man_t *pHopMan = (Hop_Man_t *)pNtk->pManFunc;
    Hop_Obj_t *pFunc   = (Hop_Obj_t *)pNode->pData;
    if (!pFunc)
        return false;
    Vec_Int_t *vTruth = Vec_IntAlloc(Kit_TruthWordNum(nFanins) * 2 + 32);
    unsigned *pTruth = Hop_ManConvertAigToTruth(pHopMan, pFunc, nFanins, vTruth, 0);
    if (!pTruth)
    {
        Vec_IntFree(vTruth);
        return false;
    }
    // nFanins <= 6 => 1 or 2 words. For <= 5 vars the single word already
    // holds the pattern replicated, so mirroring it into the high half keeps
    // every index < 2^nFanins readable with one uniform expression.
    uint64_t tt = (uint64_t)pTruth[0];
    if (Kit_TruthWordNum(nFanins) >= 2)
        tt |= (uint64_t)pTruth[1] << 32;
    else
        tt |= tt << 32;
    Vec_IntFree(vTruth);
    out = tt;
    return true;
}

std::vector<BoundaryLut> collect_boundary_luts(Abc_Ntk_t *pNtk, int dstPart, int &nSkippedWide)
{
    std::vector<BoundaryLut> out;
    Abc_Obj_t *pObj, *pFanin;
    int i, j;
    Abc_NtkForEachNode(pNtk, pObj, i)
    {
        if (!Abc_PartIdIsValid(Abc_ObjGetPartId(pObj)))
            continue;
        if ((int)Abc_ObjGetPartId(pObj) != dstPart)
            continue;

        BoundaryLut lut;
        lut.node = pObj;
        Abc_ObjForEachFanin(pObj, pFanin, j)
        {
            lut.fanins.push_back(pFanin->Id);
            part_id fp = Abc_ObjGetPartId(pFanin);
            if (Abc_PartIdIsValid(fp) && (int)fp != dstPart)
                lut.bound.push_back(pFanin->Id);
        }
        if (lut.bound.size() < 2)
            continue;                      // no joint compression possible

        if (!node_truth_u64(pObj, lut.truth))
        {
            ++nSkippedWide;                // too many fanins, or unusable node function
            continue;
        }
        std::sort(lut.bound.begin(), lut.bound.end());
        lut.bound.erase(std::unique(lut.bound.begin(), lut.bound.end()), lut.bound.end());
        if (lut.bound.size() < 2)
            continue;                      // same net feeding two fanin slots
        out.push_back(std::move(lut));
    }
    return out;
}

std::vector<Group> group_boundary_luts(const std::vector<BoundaryLut> &luts,
                                       int maxBound, int maxLuts)
{
    int n = (int)luts.size();
    std::vector<int> parent(n);
    for (int i = 0; i < n; ++i) parent[i] = i;
    std::function<int(int)> find = [&](int x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    auto uni = [&](int a, int b) { parent[find(a)] = find(b); };

    // Union LUTs that share a crossing net: net ObjId -> first LUT index seen.
    std::unordered_map<int, int> netOwner;
    for (int i = 0; i < n; ++i)
        for (int net : luts[i].bound)
        {
            auto it = netOwner.find(net);
            if (it == netOwner.end()) netOwner.emplace(net, i);
            else uni(i, it->second);
        }

    std::vector<std::vector<int>> comps;
    std::unordered_map<int, int> rootToComp;
    for (int i = 0; i < n; ++i)
    {
        int r = find(i);
        auto it = rootToComp.find(r);
        if (it == rootToComp.end()) { rootToComp.emplace(r, (int)comps.size()); comps.push_back({}); }
        comps[rootToComp[r]].push_back(i);
    }

    std::vector<Group> groups;
    for (const auto &comp : comps)
    {
        Group g;
        for (int idx : comp)
        {
            // Would adding this LUT bust either cap? If so, close the group first.
            std::vector<int> merged = g.bound_union;
            merged.insert(merged.end(), luts[idx].bound.begin(), luts[idx].bound.end());
            std::sort(merged.begin(), merged.end());
            merged.erase(std::unique(merged.begin(), merged.end()), merged.end());

            bool bustsLuts  = (int)g.luts.size() + 1 > maxLuts;
            bool bustsBound = (int)merged.size() > maxBound;
            if (!g.luts.empty() && (bustsLuts || bustsBound))
            {
                groups.push_back(std::move(g));
                g = Group{};
                merged = luts[idx].bound;    // fresh group starts from this LUT alone
            }
            g.luts.push_back(idx);
            g.bound_union = std::move(merged);
        }
        if (!g.luts.empty())
            groups.push_back(std::move(g));
    }
    return groups;
}

void classify_bound_nets(Abc_Ntk_t *pNtk, const std::vector<BoundaryLut> &luts,
                         const Group &g, int dstPart,
                         std::vector<int> &bkill, std::vector<int> &bkeep)
{
    bkill.clear();
    bkeep.clear();

    std::unordered_set<int> memberIds;
    for (int idx : g.luts)
        memberIds.insert(luts[idx].node->Id);

    for (int net : g.bound_union)
    {
        Abc_Obj_t *pNet = Abc_NtkObj(pNtk, net);
        bool covered = true;
        if (!pNet)
        {
            covered = false;               // should not happen; stay conservative
        }
        else
        {
            Abc_Obj_t *pFanout;
            int k;
            Abc_ObjForEachFanout(pNet, pFanout, k)
            {
                if (!Abc_ObjIsNode(pFanout))
                    continue;              // PO/CO carries no part_id
                part_id fp = Abc_ObjGetPartId(pFanout);
                if (!Abc_PartIdIsValid(fp) || (int)fp != dstPart)
                    continue;              // not a destination-side sink
                if (memberIds.find(pFanout->Id) == memberIds.end())
                { covered = false; break; }
            }
        }
        if (covered) bkill.push_back(net);
        else         bkeep.push_back(net);
    }
}

long joint_multiplicity(const std::vector<BoundaryLut> &luts, const Group &g,
                        const std::vector<int> &bkill)
{
    int nB = (int)bkill.size();
    if (nB == 0)
        return 1;

    // Per member, precompute where each bkill net sits in its fanin list and
    // which fanin slots stay free (locals AND B_keep nets alike -- see
    // docs/csr4.md section 7).
    struct MemberMap {
        const BoundaryLut *lut = nullptr;
        std::vector<std::pair<int, int>> boundPos;  // (index into bkill, fanin slot)
        std::vector<int> freePos;                   // fanin slots not driven by a bkill net
    };
    std::vector<MemberMap> members;
    members.reserve(g.luts.size());
    for (int idx : g.luts)
    {
        MemberMap mm;
        mm.lut = &luts[idx];
        int nFanins = (int)mm.lut->fanins.size();
        for (int slot = 0; slot < nFanins; ++slot)
        {
            int netId = mm.lut->fanins[slot];
            int bpos = -1;
            for (int j = 0; j < nB; ++j)
                if (bkill[j] == netId) { bpos = j; break; }
            if (bpos >= 0) mm.boundPos.emplace_back(bpos, slot);
            else           mm.freePos.push_back(slot);
        }
        members.push_back(std::move(mm));
    }

    std::set<std::vector<uint64_t>> seen;
    long total = 1L << nB;
    std::vector<uint64_t> key(members.size());
    for (long bAssign = 0; bAssign < total; ++bAssign)
    {
        for (size_t mi = 0; mi < members.size(); ++mi)
        {
            const MemberMap &mm = members[mi];
            // Fixed part of the minterm index contributed by the bound nets.
            int fixedIdx = 0;
            for (const auto &bp : mm.boundPos)
                if ((bAssign >> bp.first) & 1L)
                    fixedIdx |= 1 << bp.second;

            int nFree = (int)mm.freePos.size();
            uint64_t restriction = 0;
            for (int f = 0; f < (1 << nFree); ++f)
            {
                int idx = fixedIdx;
                for (int q = 0; q < nFree; ++q)
                    if ((f >> q) & 1)
                        idx |= 1 << mm.freePos[q];
                if ((mm.lut->truth >> idx) & 1ULL)
                    restriction |= 1ULL << f;
            }
            key[mi] = restriction;
        }
        seen.insert(key);
    }
    return (long)seen.size();
}

bool check_k_feasible(const std::vector<BoundaryLut> &luts, const Group &g,
                      const std::vector<int> &bkill, int t, int lutSize)
{
    std::unordered_set<int> killSet(bkill.begin(), bkill.end());
    for (int idx : g.luts)
    {
        int nFree = 0;
        for (int netId : luts[idx].fanins)
            if (killSet.find(netId) == killSet.end())
                ++nFree;
        if (t + nFree > lutSize)
            return false;
    }
    return true;
}

GroupResult evaluate_group(Abc_Ntk_t *pNtk, const std::vector<BoundaryLut> &luts,
                           const Group &g, int dstPart, const Config &cfg)
{
    GroupResult r;
    r.n_luts = (int)g.luts.size();

    std::vector<int> bkill, bkeep;
    classify_bound_nets(pNtk, luts, g, dstPart, bkill, bkeep);
    r.n_bkill = (int)bkill.size();
    r.n_bkeep = (int)bkeep.size();

    if (r.n_bkill < 2 || r.n_bkill > cfg.max_bound)
        return r;                         // nothing to compress, or too wide to enumerate

    r.mu = joint_multiplicity(luts, g, bkill);
    r.t  = ceil_log2(r.mu);
    r.k_feasible = check_k_feasible(luts, g, bkill, r.t, cfg.lut_size);
    if (r.k_feasible)
    {
        int gain = r.n_bkill - r.t;
        r.gain = gain > 0 ? gain : 0;
    }
    return r;
}

bool RunCsr4(Abc_Ntk_t *pNtk, const Config &cfg)
{
    if (!pNtk) { printf("csr4: current network is empty\n"); return false; }
    if (!Abc_NtkIsLogic(pNtk)) { printf("csr4: network must be logic (not AIG)\n"); return false; }
    if (!pNtk->pPdb) { printf("csr4: no partition database (run hpart first)\n"); return false; }
    int nParts = Abc_NtkPdb(pNtk)->num_parts();
    if (nParts != 2) {
        printf("csr4: v1 only supports N=2 partitions (got %d)\n", nParts);
        return false;
    }

    long globalGain = 0, globalEnc = 0, globalBkill = 0;
    for (int dstPart = 0; dstPart <= 1; ++dstPart)
    {
        int srcPart = 1 - dstPart;
        int nSkippedWide = 0;
        std::vector<BoundaryLut> luts = collect_boundary_luts(pNtk, dstPart, nSkippedWide);
        std::vector<Group> groups = group_boundary_luts(luts, cfg.max_bound, cfg.max_luts);

        long dirGain = 0, dirEnc = 0, dirBkill = 0;
        for (const Group &g : groups)
        {
            GroupResult r = evaluate_group(pNtk, luts, g, dstPart, cfg);
            dirBkill += r.n_bkill;
            if (r.gain <= 0)
                continue;
            dirGain += r.gain;
            dirEnc  += r.t;
            if (cfg.verbose)
                printf("  [%d->%d] group luts=%d bkill=%d bkeep=%d mu=%ld t=%d gain=%d\n",
                       srcPart, dstPart, r.n_luts, r.n_bkill, r.n_bkeep, r.mu, r.t, r.gain);
        }
        globalGain  += dirGain;
        globalEnc   += dirEnc;
        globalBkill += dirBkill;
        printf("csr4: dir %d->%d: %zu boundary LUTs, %zu groups, %d wide-skipped, "
               "sum-bkill=%ld recoverable=%ld encoders=%ld\n",
               srcPart, dstPart, luts.size(), groups.size(), nSkippedWide,
               dirBkill, dirGain, dirEnc);
    }

    // Dedup (one physical wire per net, not per crossing fanin slot) is the
    // correct denominator for "fraction of physical wires recovered" -- see
    // docs/csr4.md section 9. Abc_NtkComputeCutEdgeNum is the deduplicated
    // count: a net that fans out to several destination-side LUTs still
    // costs one physical wire, matching the "cut-edge" field ps prints.
    int cutEdge = Abc_NtkComputeCutEdgeNum(pNtk);
    double pct = cutEdge > 0 ? 100.0 * (double)globalGain / (double)cutEdge : 0.0;
    printf("csr4: TOTAL recoverable wires (detected-floor, cut-function ODC only) = %ld / %d crossing (%.1f%%)\n",
           globalGain, cutEdge, pct);
    printf("csr4: cost %ld encoder LUT(s); NOTE lower bound -- fixed cut boundaries, "
           "capped grouping, K-infeasible groups all round down.\n", globalEnc);
    (void)globalBkill;
    return true;
}

} // namespace fox::csr4
