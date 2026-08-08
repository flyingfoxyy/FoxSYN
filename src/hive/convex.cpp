#include "hive/convex.hpp"

#include <algorithm>
#include <unordered_set>

namespace fox::hive {

ClosureResult ComputeClosure(const CombGraph &g, const Region &r,
                             const std::vector<int> &added, int budget)
{
    ClosureResult res;

    std::unordered_set<int> base;
    // Region::member_ids() copies; iterate members via the sorted view once.
    for (int id : r.member_ids())
        base.insert(id);
    for (int id : added)
        base.insert(id);
    if (base.empty())
    {
        res.ok = true;
        return res;
    }

    int rank_min = 0, rank_max = 0;
    bool first = true;
    for (int id : base)
    {
        const int rk = g.rank(id);
        if (first) { rank_min = rank_max = rk; first = false; }
        else
        {
            rank_min = std::min(rank_min, rk);
            rank_max = std::max(rank_max, rk);
        }
    }

    // forward: desc within the open band (rank strictly below rank_max)
    std::unordered_set<int> desc;
    std::vector<int> stack;
    for (int id : base)
        stack.push_back(id);
    while (!stack.empty())
    {
        const int v = stack.back();
        stack.pop_back();
        for (int s : g.succs(v))
        {
            if (g.rank(s) >= rank_max)
                continue;   // rank only grows; s can never reach back into base
            if (base.count(s) || desc.count(s))
                continue;
            desc.insert(s);
            if ((int)desc.size() > budget)
                return res;   // ok=false: reject the move (spec 3.3)
            stack.push_back(s);
        }
    }

    // backward: anc within the open band, collect desc ∩ anc
    std::unordered_set<int> anc;
    for (int id : base)
        stack.push_back(id);
    while (!stack.empty())
    {
        const int v = stack.back();
        stack.pop_back();
        for (int p : g.preds(v))
        {
            if (g.rank(p) <= rank_min)
                continue;
            if (base.count(p) || anc.count(p))
                continue;
            anc.insert(p);
            if ((int)anc.size() > budget)
                return res;
            stack.push_back(p);
        }
    }

    for (int v : desc)
        if (anc.count(v))
            res.violators.push_back(v);
    std::sort(res.violators.begin(), res.violators.end());
    res.ok = true;
    return res;
}

bool IsConvexBrute(const CombGraph &g, const std::vector<int> &memberIds)
{
    // Deliberately different code path: id-indexed flag arrays over the whole
    // graph, full reachability, no band, no budget.
    std::vector<char> member(g.max_id(), 0), desc(g.max_id(), 0), anc(g.max_id(), 0);
    for (int id : memberIds)
        member[id] = 1;

    std::vector<int> stack(memberIds.begin(), memberIds.end());
    while (!stack.empty())
    {
        const int v = stack.back();
        stack.pop_back();
        for (int s : g.succs(v))
            if (!desc[s])
            {
                desc[s] = 1;
                stack.push_back(s);
            }
    }
    stack.assign(memberIds.begin(), memberIds.end());
    while (!stack.empty())
    {
        const int v = stack.back();
        stack.pop_back();
        for (int p : g.preds(v))
            if (!anc[p])
            {
                anc[p] = 1;
                stack.push_back(p);
            }
    }
    for (int id : g.vertices())
        if (!member[id] && desc[id] && anc[id])
            return false;
    return true;
}

} // namespace fox::hive
