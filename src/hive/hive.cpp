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

namespace {

int JaccardPct(const std::vector<int> &a, const std::vector<int> &b)
{
    // both sorted ascending
    size_t i = 0, j = 0;
    int inter = 0;
    while (i < a.size() && j < b.size())
    {
        if (a[i] < b[j])
            ++i;
        else if (a[i] > b[j])
            ++j;
        else
        {
            ++inter;
            ++i;
            ++j;
        }
    }
    const int uni = (int)a.size() + (int)b.size() - inter;
    return uni > 0 ? (int)(100LL * inter / uni) : 0;
}

} // namespace

Result RunHive(Abc_Ntk_t *pNtk, const Config &cfg)
{
    Result res;
    if (!pNtk || !Abc_NtkIsLogic(pNtk))
        return res;
    CombGraph g(pNtk);
    if (!g.acyclic())
    {
        printf("hive: combinational loop detected\n");
        return res;
    }
    res.ok = true;
    if (g.vertices().empty())
        return res;   // no internal nodes: normal empty result (spec 6)

    // seeds: MFFC size descending, ties by id, top num_seeds (spec 4.1)
    std::vector<std::pair<int, int>> sized;   // (mffc_size, id)
    sized.reserve(g.vertices().size());
    for (int id : g.vertices())
        // Abc_NodeMffcSize hardcodes Abc_ObjFanin0/Abc_ObjFanin1 (2-input AIG
        // only); mapped LUTs can have any fanin count, so use
        // Abc_NodeMffcLabel (generic Abc_ObjForEachFanin) with vNodes=NULL
        // to get just the size, matching what GrowFromSeed's MFFC call does.
        sized.push_back({Abc_NodeMffcLabel(Abc_NtkObj(pNtk, id), NULL), id});
    std::sort(sized.begin(), sized.end(), [](const auto &a, const auto &b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    const int nseeds = cfg.num_seeds == 0
                           ? (int)sized.size()
                           : std::min<int>(cfg.num_seeds, (int)sized.size());

    std::vector<RegionReport> cands;
    for (int k = 0; k < nseeds; ++k)
    {
        GrowResult gr = GrowFromSeed(g, sized[k].second, cfg);
        if (gr.has_region)
            cands.push_back(std::move(gr.report));
    }
    std::sort(cands.begin(), cands.end(),
              [](const RegionReport &a, const RegionReport &b) { return a.q > b.q; });

    for (RegionReport &c : cands)
    {
        if ((int)res.regions.size() >= cfg.num_regions)
            break;
        bool dup = false;
        for (const RegionReport &acc : res.regions)
            if (JaccardPct(c.member_ids, acc.member_ids) > kJaccardPct)
            {
                dup = true;
                break;
            }
        if (!dup)
            res.regions.push_back(std::move(c));
    }
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

    const int nNodes = Abc_NtkNodeNum(pNtk);
    const int nSeeds = cfg.num_seeds == 0 ? nNodes : std::min(cfg.num_seeds, nNodes);
    Result res = RunHive(pNtk, cfg);
    if (!res.ok)
        return false;

    printf("hive: %d nodes, K=%d, %d seeds\n", nNodes, cfg.lut_k, nSeeds);
    printf("hive: gap is against LB under model M1-M3 (structural-pin-preserving, LUT-only);\n");
    printf("      it is NOT a lower bound for functional resynthesis -- see docs/hive-design.md 2.5\n");
    if (res.regions.empty())
    {
        printf("hive: 0 regions\n");
        return true;
    }

    printf("  #  root          N   in  out      Q    LB   gap  rank\n");
    std::unordered_set<int> distinct;
    for (size_t idx = 0; idx < res.regions.size(); ++idx)
    {
        const RegionReport &r = res.regions[idx];
        printf("%3zu  %-12s %4d %4d %4d %6.2f %5d %5d  %d..%d\n",
               idx, Abc_ObjName(Abc_NtkObj(pNtk, r.root_id)),
               r.n, r.in, r.out, r.q, r.lb, r.gap, r.rank_min, r.rank_max);
        distinct.insert(r.member_ids.begin(), r.member_ids.end());
        if (cfg.verbose)
        {
            CombGraph g(pNtk);
            Region reg(g);
            reg.init(r.member_ids);
            printf("     members:");
            for (int id : r.member_ids)
                printf(" %d", id);
            printf("\n     in:");
            for (int id : reg.in_objects())
                printf(" %d", id);
            printf("\n     out:");
            for (int id : reg.out_members())
                printf(" %d", id);
            printf("\n");
        }
    }
    printf("hive: %zu regions, %zu distinct nodes (%.1f%% of netlist), Q in [%.2f, %.2f]\n",
           res.regions.size(), distinct.size(),
           nNodes > 0 ? 100.0 * (double)distinct.size() / nNodes : 0.0,
           res.regions.back().q, res.regions.front().q);
    return true;
}

} // namespace fox::hive
