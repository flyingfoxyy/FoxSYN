#include "csr3/csr3.hpp"
#include "csr3/csr3_internal.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <functional>
#include <unordered_set>
#include <vector>

#include "base/abc/abc.h"
#include "base/abc/abcPdb.hpp"

extern "C" {
#include "aig/aig/aig.h"
#include "aig/gia/gia.h"
#include "aig/gia/giaAig.h"    // Gia_ManFromAig (not re-exported by gia.h)
#include "sat/cnf/cnf.h"
#include "sat/bsat/satSolver.h"
Aig_Man_t * Abc_NtkToDar(Abc_Ntk_t *pNtk, int fExors, int fRegisters);
}

namespace fox::csr3 {

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

int ceil_log2(long m)
{
    if (m <= 1)
        return 0;
    int bits = 0;
    long v = m - 1;
    while (v > 0) { v >>= 1; ++bits; }
    return bits;
}

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
    assert(!lines.empty());
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

// Assemble the k-bit output tuple for pattern p (0..totalPats-1) from GIA CO sim words.
// Returns tuples counted into a hash set; k <= 63 guaranteed by the -M cap.
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
