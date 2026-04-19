#include "timer.hpp"

#include "base/abc/abc.h"
#include "base/abc/abcPdb.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace fox::timer {

static constexpr float LUT_DLY = 1.0f;
static constexpr float NET_DLY = 0.0f;
static constexpr float HOP_DLY = 200.0f;
static constexpr float EPS     = 1e-3f;

static float edge_delay(Abc_Obj_t *pFrom, Abc_Obj_t *pTo, Pdb *pPdb)
{
    if (!pPdb) return NET_DLY;
    part_id pFromPart = Abc_ObjGetPartId(pFrom);
    part_id pToPart   = Abc_ObjGetPartId(pTo);
    if (pFromPart == ABC_PART_ID_NONE || pToPart == ABC_PART_ID_NONE)
        return NET_DLY;
    return NET_DLY + ((pFromPart != pToPart) ? HOP_DLY : 0.0f);
}

SimpleTimer::SimpleTimer(Abc_Ntk_t *pNtk)
    : pNtk(pNtk)
    , pPdb(pNtk->pPdb)
{
    int nMaxId = Abc_NtkObjNumMax(pNtk);
    arrival.assign(nMaxId + 1, 0.0f);
    vTopo = Abc_NtkDfs(pNtk, 1);
}

SimpleTimer::~SimpleTimer()
{
    if (vTopo) Vec_PtrFree(vTopo);
}

void SimpleTimer::compute_arrival()
{
    // Refresh topology in case the network was mutated (e.g. replicate added
    // new nodes since construction / last call).
    if (vTopo) Vec_PtrFree(vTopo);
    vTopo = Abc_NtkDfs(pNtk, 1);
    int nMaxId = Abc_NtkObjNumMax(pNtk);
    if (static_cast<int>(arrival.size()) < nMaxId + 1)
        arrival.resize(nMaxId + 1, 0.0f);

    int i, k;
    Abc_Obj_t *pObj, *pFanin;

    Abc_NtkForEachCi(pNtk, pObj, i)
        arrival[pObj->Id] = 0.0f;

    Vec_PtrForEachEntry(Abc_Obj_t *, vTopo, pObj, i)
    {
        if (!Abc_ObjIsNode(pObj))
            continue;
        float max_fanin = 0.0f;
        Abc_ObjForEachFanin(pObj, pFanin, k)
        {
            float d = arrival[pFanin->Id] + edge_delay(pFanin, pObj, pPdb);
            if (d > max_fanin)
                max_fanin = d;
        }
        arrival[pObj->Id] = LUT_DLY + max_fanin;
    }

    // Mirror driver arrival onto COs for reporting convenience.
    Abc_Obj_t *pCo, *pDrv;
    Abc_NtkForEachCo(pNtk, pCo, i)
    {
        pDrv = Abc_ObjFanin0(pCo);
        arrival[pCo->Id] = pDrv ? arrival[pDrv->Id] : 0.0f;
    }
}

float SimpleTimer::max_arrival() const
{
    float max_arr = 0.0f;
    int i;
    Abc_Obj_t *pCo, *pDriver;
    Abc_NtkForEachCo(pNtk, pCo, i)
    {
        pDriver = Abc_ObjFanin0(pCo);
        if (pDriver && arrival[pDriver->Id] > max_arr)
            max_arr = arrival[pDriver->Id];
    }
    return max_arr;
}

void SimpleTimer::extract_critical_path(std::vector<Abc_Obj_t *> &cpath)
{
    cpath.clear();
    float max_arr = max_arrival();
    if (max_arr <= EPS)
        return;

    int nMaxId = Abc_NtkObjNumMax(pNtk);
    std::vector<char> is_crit(nMaxId + 1, 0);

    int i;
    Abc_Obj_t *pCo, *pDriver;
    Abc_NtkForEachCo(pNtk, pCo, i)
    {
        pDriver = Abc_ObjFanin0(pCo);
        if (pDriver && std::abs(arrival[pDriver->Id] - max_arr) < EPS)
            is_crit[pDriver->Id] = 1;
    }

    int nEntries = Vec_PtrSize(vTopo);
    for (int idx = nEntries - 1; idx >= 0; --idx)
    {
        Abc_Obj_t *pObj = (Abc_Obj_t *)Vec_PtrEntry(vTopo, idx);
        if (!is_crit[pObj->Id])
            continue;
        if (!Abc_ObjIsNode(pObj))
            continue;

        float target = arrival[pObj->Id] - LUT_DLY;
        Abc_Obj_t *pFanin;
        int k;
        Abc_ObjForEachFanin(pObj, pFanin, k)
        {
            float contrib = arrival[pFanin->Id] + edge_delay(pFanin, pObj, pPdb);
            if (std::abs(contrib - target) < EPS)
                is_crit[pFanin->Id] = 1;
        }
    }

    Abc_Obj_t *pObj;
    Vec_PtrForEachEntry(Abc_Obj_t *, vTopo, pObj, i)
    {
        if (is_crit[pObj->Id] && Abc_ObjIsNode(pObj))
            cpath.push_back(pObj);
    }
}

std::vector<Path> SimpleTimer::extract_top_paths(int n)
{
    std::vector<Path> out;
    if (n <= 0)
        return out;

    struct Endpoint {
        Abc_Obj_t *pCo;
        Abc_Obj_t *pDrv;
        float arr;
    };
    std::vector<Endpoint> eps;

    int i;
    Abc_Obj_t *pCo;
    Abc_NtkForEachCo(pNtk, pCo, i)
    {
        Abc_Obj_t *pDrv = Abc_ObjFanin0(pCo);
        if (!pDrv) continue;
        eps.push_back({pCo, pDrv, arrival[pDrv->Id]});
    }

    std::sort(eps.begin(), eps.end(),
              [](const Endpoint &a, const Endpoint &b) { return a.arr > b.arr; });

    int take = std::min(static_cast<int>(eps.size()), n);
    out.reserve(take);

    for (int k = 0; k < take; ++k)
    {
        Path p;
        p.delay = eps[k].arr;

        // Trace back from the CO driver, always following the max-contribution fanin.
        std::vector<int> rev_nodes;
        Abc_Obj_t *pCur = eps[k].pDrv;
        while (pCur && Abc_ObjIsNode(pCur))
        {
            rev_nodes.push_back(pCur->Id);
            Abc_Obj_t *pBest = nullptr;
            float best = -1.0f;
            Abc_Obj_t *pFanin;
            int m;
            Abc_ObjForEachFanin(pCur, pFanin, m)
            {
                float contrib = arrival[pFanin->Id] + edge_delay(pFanin, pCur, pPdb);
                if (contrib > best) { best = contrib; pBest = pFanin; }
            }
            if (!pBest) break;
            pCur = pBest;
        }

        if (pCur)
            p.ids.push_back(pCur->Id); // startpoint (PI / CI)
        for (auto it = rev_nodes.rbegin(); it != rev_nodes.rend(); ++it)
            p.ids.push_back(*it);
        p.ids.push_back(eps[k].pCo->Id); // endpoint (PO / CO)

        out.push_back(std::move(p));
    }
    return out;
}

void SimpleTimer::recompute_cone(Abc_Obj_t *pRoot)
{
    // pRoot may be a freshly-created node (e.g. from replicate) that is not in
    // vTopo yet — grow the arrival table and compute its value explicitly.
    if (static_cast<int>(arrival.size()) <= pRoot->Id)
        arrival.resize(pRoot->Id + 1, 0.0f);

    Abc_NtkIncrementTravId(pNtk);
    mark_fanout_cone(pRoot);

    if (Abc_ObjIsNode(pRoot))
    {
        float max_fanin = 0.0f;
        Abc_Obj_t *pFanin;
        int k;
        Abc_ObjForEachFanin(pRoot, pFanin, k)
        {
            float d = arrival[pFanin->Id] + edge_delay(pFanin, pRoot, pPdb);
            if (d > max_fanin)
                max_fanin = d;
        }
        arrival[pRoot->Id] = LUT_DLY + max_fanin;
    }

    int i, k;
    Abc_Obj_t *pObj, *pFanin;
    Vec_PtrForEachEntry(Abc_Obj_t *, vTopo, pObj, i)
    {
        if (!Abc_NodeIsTravIdCurrent(pObj))
            continue;
        if (Abc_ObjIsCi(pObj))
            continue;
        if (!Abc_ObjIsNode(pObj))
            continue;
        if (pObj == pRoot) // already handled above
            continue;

        float max_fanin = 0.0f;
        Abc_ObjForEachFanin(pObj, pFanin, k)
        {
            float d = arrival[pFanin->Id] + edge_delay(pFanin, pObj, pPdb);
            if (d > max_fanin)
                max_fanin = d;
        }
        arrival[pObj->Id] = LUT_DLY + max_fanin;
    }

    // Mirror driver arrival onto COs inside the cone.
    Abc_Obj_t *pCo, *pDrv;
    Abc_NtkForEachCo(pNtk, pCo, i)
    {
        if (!Abc_NodeIsTravIdCurrent(pCo))
            continue;
        pDrv = Abc_ObjFanin0(pCo);
        arrival[pCo->Id] = pDrv ? arrival[pDrv->Id] : 0.0f;
    }
}

void SimpleTimer::mark_fanout_cone(Abc_Obj_t *pObj)
{
    Abc_NodeSetTravIdCurrent(pObj);
    Abc_Obj_t *pFanout;
    int i;
    Abc_ObjForEachFanout(pObj, pFanout, i)
    {
        if (!Abc_NodeIsTravIdCurrent(pFanout))
            mark_fanout_cone(pFanout);
    }
}

std::vector<Path> AnalyzeCriticalPaths(Abc_Ntk_t *pNtk, int top_n)
{
    if (!pNtk || top_n <= 0) return {};
    SimpleTimer t(pNtk);
    t.compute_arrival();
    return t.extract_top_paths(top_n);
}

static const char *kind_of(Abc_Obj_t *pObj)
{
    if (!pObj) return "?";
    if (Abc_ObjIsPi(pObj))  return "PI";
    if (Abc_ObjIsPo(pObj))  return "PO";
    if (Abc_ObjIsCi(pObj))  return "CI";
    if (Abc_ObjIsCo(pObj))  return "CO";
    if (Abc_ObjIsNode(pObj)) return "Node";
    return "?";
}

bool RunTimer(Abc_Ntk_t *pNtk, const Config &cfg)
{
    if (!pNtk) {
        printf("timer: network is null\n");
        return false;
    }
    if (Abc_NtkIsStrash(pNtk)) {
        printf("timer: does not support AIG networks (run mapping first)\n");
        return false;
    }

    SimpleTimer t(pNtk);
    t.compute_arrival();
    float max_arr = t.max_arrival();

    int n = cfg.top_n > 0 ? cfg.top_n : 1;
    auto paths = t.extract_top_paths(n);

    printf("timer: max arrival = %.2f\n", max_arr);
    printf("timer: top %zu critical path(s):\n", paths.size());

    const auto &arr = t.get_arrival();
    for (size_t idx = 0; idx < paths.size(); ++idx)
    {
        const Path &p = paths[idx];
        printf("  [%zu] delay=%.2f  length=%zu\n",
               idx, p.delay, p.ids.size());
        for (size_t j = 0; j < p.ids.size(); ++j)
        {
            int id = p.ids[j];
            Abc_Obj_t *pObj = Abc_NtkObj(pNtk, id);
            const char *name = pObj ? Abc_ObjName(pObj) : "?";
            part_id pid = pObj ? Abc_ObjGetPartId(pObj) : ABC_PART_ID_NONE;
            float a = (id >= 0 && (size_t)id < arr.size()) ? arr[id] : 0.0f;
            if (pid == ABC_PART_ID_NONE)
                printf("    %3zu  %-4s  id=%-5d  arr=%7.2f  %s\n",
                       j, kind_of(pObj), id, a, name);
            else
                printf("    %3zu  %-4s  id=%-5d  arr=%7.2f  part=%d  %s\n",
                       j, kind_of(pObj), id, a, (int)pid, name);
        }
    }

    if (cfg.verbose)
        printf("timer: done\n");
    return true;
}

} // namespace fox::timer
