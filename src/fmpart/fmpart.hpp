#ifndef FMPART_FMPART_HPP
#define FMPART_FMPART_HPP

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <random>
#include <ranges>
#include <span>
#include <vector>

#include "fmpart/fm_buckets.hpp"

namespace fox::fmpart {

// Extra contracts beyond the concept (docs/fmpart-design.md §3.1):
//  - vertex ids cover [0, num_vertices()) contiguously; net ids cover [0, num_nets())
//  - pins_of(e) yields valid vertex ids with no duplicates
//  - vertex_weight(v) >= 0, net_weight(e) >= 0
//  - all members are called only during FMPart construction and must be re-entrant
template <typename G>
concept FMHypergraph = requires(const G &g, int v, int e) {
    { g.num_vertices()   } -> std::convertible_to<int>;
    { g.num_nets()       } -> std::convertible_to<int>;
    { g.vertex_weight(v) } -> std::convertible_to<int>;
    { g.net_weight(e)    } -> std::convertible_to<int>;
    { g.pins_of(e)       } -> std::ranges::input_range;
};

struct Config {
    int      balance_pct = 2;    // balance slack percent; same semantics as cpr.cpp:254
    int      max_passes  = 10;   // max number of passes
    int      min_gain    = 1;    // stop when a pass gain is < min_gain
    unsigned seed        = 1;    // seed for random initial solution
    bool     verbose     = false;
    bool     self_check  = false; // test only: O(pins) invariant check after each move
};

struct Result {
    std::vector<uint8_t> part;        // partition of each vertex, 0 or 1
    int  cut         = 0;             // final cut: sum of weights of cut nets
    int  initial_cut = 0;             // cut before optimization
    int  passes      = 0;             // number of passes actually run
    bool balanced    = false;         // whether the final solution meets the balance constraint
    int  self_check_failures = 0;     // mismatch count found when cfg.self_check is on
};

template <FMHypergraph G>
class FMPart {
public:
    FMPart(const G &g, const Config &cfg)
        : m_cfg(cfg)
    {
        m_nv = static_cast<int>(g.num_vertices());
        m_ne = static_cast<int>(g.num_nets());

        m_vw.resize(m_nv);
        for (int v = 0; v < m_nv; ++v)
            m_vw[v] = static_cast<int>(g.vertex_weight(v));
        m_nw.resize(m_ne);
        for (int e = 0; e < m_ne; ++e)
            m_nw[e] = static_cast<int>(g.net_weight(e));

        // net -> vertices CSR (spec §4.1)
        m_pin_start.assign(m_ne + 1, 0);
        for (int e = 0; e < m_ne; ++e)
            for (auto pv : g.pins_of(e)) {
                (void)pv;
                ++m_pin_start[e + 1];
            }
        for (int e = 0; e < m_ne; ++e)
            m_pin_start[e + 1] += m_pin_start[e];
        m_pin_list.resize(m_pin_start[m_ne]);
        {
            std::vector<int> fill(m_ne, 0);
            for (int e = 0; e < m_ne; ++e)
                for (auto pv : g.pins_of(e)) {
                    const int v = static_cast<int>(pv);
                    assert(v >= 0 && v < m_nv);
                    m_pin_list[m_pin_start[e] + fill[e]++] = v;
                }
        }

        // vertex -> nets CSR (transpose)
        m_net_start.assign(m_nv + 1, 0);
        for (int idx = 0; idx < (int)m_pin_list.size(); ++idx)
            ++m_net_start[m_pin_list[idx] + 1];
        for (int v = 0; v < m_nv; ++v)
            m_net_start[v + 1] += m_net_start[v];
        m_net_list.resize(m_pin_list.size());
        {
            std::vector<int> fill(m_nv, 0);
            for (int e = 0; e < m_ne; ++e)
                for (int idx = m_pin_start[e]; idx < m_pin_start[e + 1]; ++idx) {
                    const int v = m_pin_list[idx];
                    m_net_list[m_net_start[v] + fill[v]++] = e;
                }
        }

        // balance upper bound; cpr.cpp:254 semantics (spec §5.1)
        m_total_weight = 0;
        for (int v = 0; v < m_nv; ++v)
            m_total_weight += m_vw[v];
        const int avg = m_total_weight / 2;
        const int slack = (avg * m_cfg.balance_pct + 99) / 100;
        m_max_weight = std::max(avg + slack, avg + 1);

        m_min_vw = m_nv > 0 ? *std::min_element(m_vw.begin(), m_vw.end()) : 0;

        // Gmax = max_v Σ weights of incident nets (spec §4.4)
        m_gmax = 0;
        for (int v = 0; v < m_nv; ++v) {
            int s = 0;
            for (int idx = m_net_start[v]; idx < m_net_start[v + 1]; ++idx)
                s += m_nw[m_net_list[idx]];
            m_gmax = std::max(m_gmax, s);
        }
    }

    int max_weight() const { return m_max_weight; }

    Result run(std::span<const uint8_t> init = {}, std::span<const int8_t> fixed = {})
    {
        assert(init.empty() || (int)init.size() == m_nv);
        assert(fixed.empty() || (int)fixed.size() == m_nv);

        m_part.assign(m_nv, 0);
        m_fixed.assign(m_nv, int8_t{-1});
        if (!fixed.empty())
            std::copy(fixed.begin(), fixed.end(), m_fixed.begin());

        if (!init.empty()) {
            for (int v = 0; v < m_nv; ++v)
                m_part[v] = m_fixed[v] >= 0 ? (uint8_t)m_fixed[v] : (init[v] ? 1 : 0);
        } else {
            random_init();
        }

        rebuild_counts();
        m_check_failures = 0;

        Result res;
        res.initial_cut = m_cut;

        for (int p = 1; p <= m_cfg.max_passes; ++p) {
            const bool was_balanced = is_balanced();
            const int g = run_one_pass();
            res.passes = p;
            const bool now_balanced = is_balanced();
            if (m_cfg.verbose)
                std::printf("fmpart: pass %d gain %d cut %d w0 %d w1 %d\n",
                            p, g, m_cut, m_wsum[0], m_wsum[1]);
            // balance-repair pass (false -> true) does not count toward convergence; see spec §5.3
            if (!(now_balanced && !was_balanced) && g < m_cfg.min_gain)
                break;
        }

        res.part = m_part;
        res.cut = m_cut;
        res.balanced = is_balanced();
        res.self_check_failures = m_check_failures;
        return res;
    }

private:
    // Place fixed vertices first; free vertices go greedily to the lighter side in random order
    void random_init()
    {
        std::mt19937 rng(m_cfg.seed);
        std::vector<int> order;
        order.reserve(m_nv);
        int w[2] = {0, 0};
        for (int v = 0; v < m_nv; ++v) {
            if (m_fixed[v] >= 0) {
                m_part[v] = (uint8_t)m_fixed[v];
                w[m_part[v]] += m_vw[v];
            } else {
                order.push_back(v);
            }
        }
        std::shuffle(order.begin(), order.end(), rng);
        for (int v : order) {
            const int side = w[0] <= w[1] ? 0 : 1;
            m_part[v] = (uint8_t)side;
            w[side] += m_vw[v];
        }
    }

    // Rebuild cnt / wsum / cut from m_part
    void rebuild_counts()
    {
        m_cnt.assign(2 * m_ne, 0);
        m_wsum[0] = m_wsum[1] = 0;
        for (int v = 0; v < m_nv; ++v)
            m_wsum[m_part[v]] += m_vw[v];
        for (int e = 0; e < m_ne; ++e)
            for (int idx = m_pin_start[e]; idx < m_pin_start[e + 1]; ++idx)
                ++m_cnt[2 * e + m_part[m_pin_list[idx]]];
        m_cut = 0;
        for (int e = 0; e < m_ne; ++e)
            if (m_cnt[2 * e] > 0 && m_cnt[2 * e + 1] > 0)
                m_cut += m_nw[e];
    }

    bool is_balanced() const
    {
        return m_wsum[0] <= m_max_weight && m_wsum[1] <= m_max_weight;
    }

    // Recompute gain of v from scratch per spec §4.3
    int compute_gain(int v) const
    {
        const int F = m_part[v], T = 1 - F;
        int gain = 0;
        for (int idx = m_net_start[v]; idx < m_net_start[v + 1]; ++idx) {
            const int e = m_net_list[idx];
            if (m_cnt[2 * e + F] == 1) gain += m_nw[e];
            if (m_cnt[2 * e + T] == 0) gain -= m_nw[e];
        }
        return gain;
    }

    // First vertex from the top of side's buckets that can move to the other side;
    // early-out via min_vw, see spec §5.2
    int pick_from(int side)
    {
        const int T = 1 - side;
        if (m_wsum[T] + m_min_vw > m_max_weight)
            return GainBuckets::kNone;
        const int cap = m_max_weight - m_wsum[T];
        return m_buckets.find_top(side, [&](int v) { return m_vw[v] <= cap; });
    }

    // Move v to the other side: lock, incremental cnt/wsum/cut, two-pass neighbor gain
    // update (spec §5.4). Must be two passes: pre-move branches need m_part[v] still on F;
    // post-move branches need it already on T.
    void move_vertex(int v)
    {
        const int F = m_part[v], T = 1 - F;
        m_locked[v] = 1;
        m_buckets.erase(v);

        // Pass 1: read pre-move counts; v still on F
        for (int idx = m_net_start[v]; idx < m_net_start[v + 1]; ++idx) {
            const int e = m_net_list[idx];
            const int cF = m_cnt[2 * e + F], cT = m_cnt[2 * e + T];
            if (cT == 0) {
                if (cF > 1)
                    m_cut += m_nw[e];                 // net becomes cut
                for (int j = m_pin_start[e]; j < m_pin_start[e + 1]; ++j) {
                    const int u = m_pin_list[j];
                    if (!m_locked[u])
                        m_buckets.update_gain(u, m_buckets.gain_of(u) + m_nw[e]);
                }
            } else if (cT == 1) {
                for (int j = m_pin_start[e]; j < m_pin_start[e + 1]; ++j) {
                    const int u = m_pin_list[j];
                    if (m_part[u] == T) {             // sole pin on T; cannot be v
                        if (!m_locked[u])
                            m_buckets.update_gain(u, m_buckets.gain_of(u) - m_nw[e]);
                        break;
                    }
                }
            }
            if (cT > 0 && cF == 1)
                m_cut -= m_nw[e];                     // net becomes uncut
            m_cnt[2 * e + F] -= 1;
            m_cnt[2 * e + T] += 1;
        }

        m_part[v] = (uint8_t)T;
        m_wsum[F] -= m_vw[v];
        m_wsum[T] += m_vw[v];

        // Pass 2: read post-move counts; v already on T
        for (int idx = m_net_start[v]; idx < m_net_start[v + 1]; ++idx) {
            const int e = m_net_list[idx];
            const int cF = m_cnt[2 * e + F];
            if (cF == 0) {
                for (int j = m_pin_start[e]; j < m_pin_start[e + 1]; ++j) {
                    const int u = m_pin_list[j];
                    if (!m_locked[u])
                        m_buckets.update_gain(u, m_buckets.gain_of(u) - m_nw[e]);
                }
            } else if (cF == 1) {
                for (int j = m_pin_start[e]; j < m_pin_start[e + 1]; ++j) {
                    const int u = m_pin_list[j];
                    if (m_part[u] == F) {             // sole pin on F; cannot be v
                        if (!m_locked[u])
                            m_buckets.update_gain(u, m_buckets.gain_of(u) + m_nw[e]);
                        break;
                    }
                }
            }
        }
    }

    // Undo one move: restore only part/cnt/wsum/cut, leave gains alone
    // (recomputed from scratch next pass, spec §5.3)
    void undo_move(int v)
    {
        const int T = m_part[v], F = 1 - T;   // v is on T now; send it back to F
        for (int idx = m_net_start[v]; idx < m_net_start[v + 1]; ++idx) {
            const int e = m_net_list[idx];
            const int cF = m_cnt[2 * e + F], cT = m_cnt[2 * e + T];
            if (cF == 0 && cT > 1)
                m_cut += m_nw[e];
            if (cF > 0 && cT == 1)
                m_cut -= m_nw[e];
            m_cnt[2 * e + T] -= 1;
            m_cnt[2 * e + F] += 1;
        }
        m_part[v] = (uint8_t)F;
        m_wsum[T] -= m_vw[v];
        m_wsum[F] += m_vw[v];
    }

    // Invariant self-check: returns mismatch count (0 = clean); prints each to stderr.
    // check_gain is true only after a move and before rollback (when incremental gains /
    // buckets are well-defined).
    int verify_invariants(bool check_gain)
    {
        int bad = 0;
        std::vector<int> cnt(2 * m_ne, 0);
        for (int e = 0; e < m_ne; ++e)
            for (int idx = m_pin_start[e]; idx < m_pin_start[e + 1]; ++idx)
                ++cnt[2 * e + m_part[m_pin_list[idx]]];
        for (int i = 0; i < 2 * m_ne; ++i)
            if (cnt[i] != m_cnt[i]) {
                std::fprintf(stderr, "fmpart: cnt mismatch at %d\n", i);
                ++bad;
                break;
            }
        int cut = 0;
        for (int e = 0; e < m_ne; ++e)
            if (cnt[2 * e] > 0 && cnt[2 * e + 1] > 0)
                cut += m_nw[e];
        if (cut != m_cut) {
            std::fprintf(stderr, "fmpart: cut %d != tracked %d\n", cut, m_cut);
            ++bad;
        }
        int w[2] = {0, 0};
        for (int v = 0; v < m_nv; ++v)
            w[m_part[v]] += m_vw[v];
        if (w[0] != m_wsum[0] || w[1] != m_wsum[1]) {
            std::fprintf(stderr, "fmpart: wsum mismatch\n");
            ++bad;
        }
        if (check_gain) {
            for (int v = 0; v < m_nv; ++v) {
                if (m_locked[v]) {
                    if (m_buckets.contains(v)) {
                        std::fprintf(stderr, "fmpart: locked vertex %d in bucket\n", v);
                        ++bad;
                    }
                    continue;
                }
                if (!m_buckets.contains(v)) {
                    std::fprintf(stderr, "fmpart: free vertex %d missing from bucket\n", v);
                    ++bad;
                    continue;
                }
                const int gexp = compute_gain(v);
                if (m_buckets.gain_of(v) != gexp || m_buckets.side_of(v) != m_part[v]) {
                    std::fprintf(stderr, "fmpart: gain/side mismatch at %d (have %d/%d want %d/%d)\n",
                                 v, m_buckets.gain_of(v), m_buckets.side_of(v), gexp, (int)m_part[v]);
                    ++bad;
                }
            }
            bad += m_buckets.check_consistency();
        }
        return bad;
    }

    // One FM pass (spec §5.3); returns cumulative gain of the accepted prefix
    int run_one_pass()
    {
        m_locked.assign(m_nv, 0);
        for (int v = 0; v < m_nv; ++v)
            if (m_fixed[v] >= 0)
                m_locked[v] = 1;
        m_buckets.reset(m_nv, m_gmax);
        for (int v = 0; v < m_nv; ++v)
            if (!m_locked[v])
                m_buckets.insert(v, m_part[v], compute_gain(v));

        m_trail.clear();
        int cum = 0;
        int best_prefix = 0;
        int best_cum = 0;
        bool best_balanced = is_balanced();   // prefix key: (balanced, cum) lexicographic

        for (;;) {
            int side;
            bool forced = false;
            if (m_wsum[0] > m_max_weight) {
                side = 0;
                forced = true;
            } else if (m_wsum[1] > m_max_weight) {
                side = 1;
                forced = true;
            } else {
                if (m_buckets.empty(0) && m_buckets.empty(1))
                    break;
                if (m_buckets.empty(1))
                    side = 0;
                else if (m_buckets.empty(0))
                    side = 1;
                else {
                    const int g0 = m_buckets.max_gain(0), g1 = m_buckets.max_gain(1);
                    if (g0 != g1)
                        side = g0 > g1 ? 0 : 1;
                    else
                        side = m_wsum[0] >= m_wsum[1] ? 0 : 1;
                }
            }

            int chosen = pick_from(side);
            if (chosen == GainBuckets::kNone) {
                if (forced)
                    break;
                chosen = pick_from(1 - side);
                if (chosen == GainBuckets::kNone)
                    break;
            }

            cum += m_buckets.gain_of(chosen);
            move_vertex(chosen);
            m_trail.push_back(chosen);

            const bool bal = is_balanced();
            if ((bal && !best_balanced) || (bal == best_balanced && cum > best_cum)) {
                best_balanced = bal;
                best_cum = cum;
                best_prefix = (int)m_trail.size();
            }

            if (m_cfg.self_check)
                m_check_failures += verify_invariants(true);
        }

        for (int i = (int)m_trail.size() - 1; i >= best_prefix; --i)
            undo_move(m_trail[i]);

        if (m_cfg.self_check)
            m_check_failures += verify_invariants(false);

        return best_cum;
    }

    // ---- graph snapshot (read-only after construction) ----
    Config m_cfg;
    int m_nv = 0, m_ne = 0;
    std::vector<int> m_vw, m_nw;
    std::vector<int> m_pin_start, m_pin_list;
    std::vector<int> m_net_start, m_net_list;
    int m_total_weight = 0;
    int m_max_weight = 0;
    int m_min_vw = 0;
    int m_gmax = 0;

    // ---- FM state reset on every run() ----
    std::vector<uint8_t> m_part;
    std::vector<int8_t> m_fixed;
    std::vector<int> m_cnt;              // flat [2*e + side]
    std::vector<char> m_locked;
    int m_wsum[2] = {0, 0};
    int m_cut = 0;
    std::vector<int> m_trail;
    GainBuckets m_buckets;
    int m_check_failures = 0;
};

} // namespace fox::fmpart

#endif // FMPART_FMPART_HPP
