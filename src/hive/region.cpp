#include "hive/region.hpp"

#include <algorithm>
#include <unordered_set>

namespace fox::hive {

void Region::init(const std::vector<int> &memberIds)
{
    m_members.clear();
    m_driven_cnt.clear();
    m_ext_succ.clear();
    m_in = 0;
    m_out = 0;
    for (int id : memberIds)
        add(id);
}

void Region::add(int id)
{
    const CombGraph &g = *m_g;
    m_members.insert(id);

    // id stops being an external driver
    auto it = m_driven_cnt.find(id);
    if (it != m_driven_cnt.end())
    {
        m_driven_cnt.erase(it);
        --m_in;
    }

    // id's drivers gain one driven member
    auto bump = [&](int d)
    {
        int &c = m_driven_cnt[d];
        if (++c == 1)
            ++m_in;
    };
    for (int p : g.preds(id))
        if (!contains(p))
            bump(p);
    for (int x : g.ext_ins(id))
        bump(x);   // PI/BO: never members

    // member predecessors lose one external successor
    for (int p : g.preds(id))
    {
        if (!contains(p) || p == id)
            continue;
        int &c = m_ext_succ[p];
        --c;
        if (c == 0 && !g.has_ext_out(p))
            --m_out;   // p leaves the out set
    }

    // id's own out status
    int cnt = 0;
    for (int s : g.succs(id))
        if (!contains(s))
            ++cnt;
    m_ext_succ[id] = cnt;
    if (g.has_ext_out(id) || cnt > 0)
        ++m_out;
}

std::vector<int> Region::member_ids() const
{
    std::vector<int> v(m_members.begin(), m_members.end());
    std::sort(v.begin(), v.end());
    return v;
}

std::vector<int> Region::in_objects() const
{
    std::vector<int> v;
    v.reserve(m_driven_cnt.size());
    for (const auto &kv : m_driven_cnt)
        v.push_back(kv.first);
    std::sort(v.begin(), v.end());
    return v;
}

std::vector<int> Region::entrance_candidates() const
{
    std::vector<int> v;
    for (const auto &kv : m_driven_cnt)
        if (m_g->is_vertex(kv.first))
            v.push_back(kv.first);
    std::sort(v.begin(), v.end());
    return v;
}

std::vector<int> Region::out_members() const
{
    std::vector<int> v;
    for (const auto &kv : m_ext_succ)
        if (m_g->has_ext_out(kv.first) || kv.second > 0)
            v.push_back(kv.first);
    std::sort(v.begin(), v.end());
    return v;
}

Metrics Region::metrics(int lut_k) const
{
    Metrics m;
    m.n = (int)m_members.size();
    m.in = m_in;
    m.out = m_out;
    const int b = m.in + m.out;
    m.q = b > 0 ? (double)m.n / b : 0.0;
    bool first = true;
    for (int id : m_members)
    {
        const int rk = m_g->rank(id);
        if (first) { m.rank_min = m.rank_max = rk; first = false; }
        else
        {
            m.rank_min = std::min(m.rank_min, rk);
            m.rank_max = std::max(m.rank_max, rk);
        }
    }
    m.lb = lower_bound_luts(lut_k);
    m.gap = m.n - m.lb;
    return m;
}

Metrics Region::recompute(int lut_k) const
{
    // From-scratch scan; deliberately shares no state with the incremental
    // bookkeeping so tests can cross-check it (docs/hive-design.md 8).
    Metrics m;
    m.n = (int)m_members.size();
    std::unordered_set<int> drivers;
    for (int id : m_members)
    {
        for (int p : m_g->preds(id))
            if (!m_members.count(p))
                drivers.insert(p);
        for (int x : m_g->ext_ins(id))
            drivers.insert(x);
        bool is_out = m_g->has_ext_out(id);
        if (!is_out)
            for (int s : m_g->succs(id))
                if (!m_members.count(s)) { is_out = true; break; }
        if (is_out)
            ++m.out;
    }
    m.in = (int)drivers.size();
    const int b = m.in + m.out;
    m.q = b > 0 ? (double)m.n / b : 0.0;
    bool first = true;
    for (int id : m_members)
    {
        const int rk = m_g->rank(id);
        if (first) { m.rank_min = m.rank_max = rk; first = false; }
        else
        {
            m.rank_min = std::min(m.rank_min, rk);
            m.rank_max = std::max(m.rank_max, rk);
        }
    }
    m.lb = lower_bound_luts(lut_k);
    m.gap = m.n - m.lb;
    return m;
}

int Region::lower_bound_luts(int lut_k) const
{
    const CombGraph &g = *m_g;
    const int km1 = lut_k - 1;
    auto ceil_div_or_0 = [km1](int x)
    {
        return x > 0 ? (x + km1 - 1) / km1 : 0;
    };

    int term1 = m_out;
    int term2 = ceil_div_or_0(m_in - m_out);

    // term3: per-output structural support inside the region (spec 2.4)
    int term3 = 0;
    for (const auto &kv : m_ext_succ)
    {
        const int j = kv.first;
        if (!(g.has_ext_out(j) || kv.second > 0))
            continue;   // not an output
        std::unordered_set<int> supp, seen;
        std::vector<int> stack{j};
        seen.insert(j);
        while (!stack.empty())
        {
            const int v = stack.back();
            stack.pop_back();
            for (int p : g.preds(v))
            {
                if (m_members.count(p))
                {
                    if (seen.insert(p).second)
                        stack.push_back(p);
                }
                else
                    supp.insert(p);
            }
            for (int x : g.ext_ins(v))
                supp.insert(x);
        }
        term3 = std::max(term3, ceil_div_or_0((int)supp.size() - 1));
    }

    return std::max(term1, std::max(term2, term3));
}

} // namespace fox::hive
