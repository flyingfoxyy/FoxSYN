#include "hive/hive.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>
#include <vector>

#include "base/abc/abc.h"
#include "hive/convex.hpp"
#include "hive/hive_graph.hpp"
#include "hive/hive_internal.hpp"
#include "hive/region.hpp"

namespace fox::hive {

namespace {

bool CapsOk(const Metrics &m, const Config &cfg)
{
    return m.n <= cfg.max_nodes && m.in <= cfg.max_in && m.out <= cfg.max_out;
}

RegionReport MakeReport(const Region &r, const Metrics &m, int rootId)
{
    RegionReport rep;
    rep.root_id = rootId;
    rep.member_ids = r.member_ids();
    rep.n = m.n;
    rep.in = m.in;
    rep.out = m.out;
    rep.lb = m.lb;
    rep.gap = m.gap;
    rep.rank_min = m.rank_min;
    rep.rank_max = m.rank_max;
    rep.q = m.q;
    return rep;
}

} // namespace

GrowResult GrowFromSeed(const CombGraph &g, int rootId, const Config &cfg)
{
    GrowResult out;
    Abc_Obj_t *pRoot = Abc_NtkObj(g.ntk(), rootId);
    if (!pRoot || !Abc_ObjIsNode(pRoot))
        return out;

    Vec_Ptr_t *vNodes = Vec_PtrAlloc(16);
    Abc_NodeMffcLabel(pRoot, vNodes);
    std::vector<int> seedIds;
    Abc_Obj_t *pObj;
    int i;
    Vec_PtrForEachEntry(Abc_Obj_t *, vNodes, pObj, i)
        seedIds.push_back((int)Abc_ObjId(pObj));
    Vec_PtrFree(vNodes);
    if (seedIds.empty())
        return out;

    Region r(g);
    r.init(seedIds);
    Metrics m = r.metrics(cfg.lut_k);
    if (!CapsOk(m, cfg))
        return out;   // oversized seed: skip, no best-prefix (spec 4.1)

    Region bestR = r;
    Metrics bestM = m;
    int stall = 0;

    while (r.size() < cfg.max_nodes)
    {
        // candidates: entrance (external vertex drivers) + exit (sampled
        // fanouts of out members), deduped by id (spec 4.2, 4.6)
        std::vector<int> cands = r.entrance_candidates();
        {
            std::unordered_set<int> seen(cands.begin(), cands.end());
            for (int om : r.out_members())
            {
                int taken = 0;
                for (int s : g.succs(om))
                {
                    if (taken >= kFanoutCap)
                        break;
                    if (r.contains(s) || seen.count(s))
                        continue;
                    seen.insert(s);
                    cands.push_back(s);
                    ++taken;
                }
            }
        }
        if (cands.empty())
            break;

        // optimistic pass: empty-closure trial per candidate (spec 4.3)
        struct Scored
        {
            int id;
            double q;
        };
        std::vector<Scored> scored;
        scored.reserve(cands.size());
        for (int c : cands)
        {
            Region t = r;
            t.add(c);
            scored.push_back({c, t.metrics(cfg.lut_k).q});
        }
        const int topl = std::min<int>(kTopL, (int)scored.size());
        std::nth_element(scored.begin(), scored.begin() + topl, scored.end(),
                         [](const Scored &a, const Scored &b) { return a.q > b.q; });

        // exact pass on the top L
        bool moved = false;
        Region nextR(g);
        Metrics nextM;
        double nextQ = -1.0;
        for (int k = 0; k < topl; ++k)
        {
            ClosureResult cl = ComputeClosure(g, r, {scored[k].id}, kClosureBudget);
            if (!cl.ok)
                continue;   // budget exceeded: reject this move (spec 3.3)
            Region t = r;
            t.add(scored[k].id);
            for (int v : cl.violators)
                t.add(v);
            Metrics tm = t.metrics(cfg.lut_k);
            if (!CapsOk(tm, cfg))
                continue;   // caps bind on intermediate states (spec 4.4)
            if (tm.q > nextQ)
            {
                nextQ = tm.q;
                nextR = std::move(t);
                nextM = tm;
                moved = true;
            }
        }
        if (!moved)
            break;

        r = std::move(nextR);
        m = nextM;
        if (m.q > bestM.q)
        {
            bestR = r;
            bestM = m;
            stall = 0;
        }
        else if (++stall >= kStall)
            break;
    }

    out.has_region = true;
    out.report = MakeReport(bestR, bestM, rootId);
    return out;
}

Result RunHive(Abc_Ntk_t *pNtk, const Config &cfg)
{
    (void)cfg;
    Result res;
    if (!pNtk || !Abc_NtkIsLogic(pNtk))
        return res;
    res.ok = true;
    return res;
}

bool ApplyHive(Abc_Frame_t *pAbc, const Config &cfg)
{
    Abc_Ntk_t *pNtk = Abc_FrameReadNtk(pAbc);
    if (!pNtk)
    {
        printf("hive: network is null\n");
        return false;
    }
    if (!Abc_NtkIsLogic(pNtk))
    {
        printf("hive: network must be logic (run if -K first)\n");
        return false;
    }
    Result res = RunHive(pNtk, cfg);
    return res.ok;
}

} // namespace fox::hive
