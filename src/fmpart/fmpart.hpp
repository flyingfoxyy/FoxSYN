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

// concept 之外的契约（docs/fmpart-design.md §3.1）：
//  - 顶点 id 连续覆盖 [0, num_vertices())，net id 连续覆盖 [0, num_nets())
//  - pins_of(e) 产出合法顶点 id，内部无重复
//  - vertex_weight(v) >= 0，net_weight(e) >= 0
//  - 所有成员仅在 FMPart 构造期间被调用，需可重入
template <typename G>
concept FMHypergraph = requires(const G &g, int v, int e) {
    { g.num_vertices()   } -> std::convertible_to<int>;
    { g.num_nets()       } -> std::convertible_to<int>;
    { g.vertex_weight(v) } -> std::convertible_to<int>;
    { g.net_weight(e)    } -> std::convertible_to<int>;
    { g.pins_of(e)       } -> std::ranges::input_range;
};

struct Config {
    int      balance_pct = 2;    // 平衡松弛百分比，语义同 cpr.cpp:254
    int      max_passes  = 10;   // pass 数上限
    int      min_gain    = 1;    // 一趟收益 < min_gain 即收敛退出
    unsigned seed        = 1;    // 随机初始解种子
    bool     verbose     = false;
    bool     self_check  = false; // 仅测试用：每次移动后跑 O(pins) 不变量检查
};

struct Result {
    std::vector<uint8_t> part;        // 每个顶点所属分区，0 或 1
    int  cut         = 0;             // 最终 cut：被切开的 net 权重和
    int  initial_cut = 0;             // 优化前的 cut
    int  passes      = 0;             // 实际执行的 pass 数
    bool balanced    = false;         // 最终解是否满足平衡约束
    int  self_check_failures = 0;     // cfg.self_check 发现的不一致计数
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

        // net -> 顶点 CSR（spec §4.1）
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

        // 顶点 -> net CSR（转置）
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

        // 平衡上界，cpr.cpp:254 语义（spec §5.1）
        m_total_weight = 0;
        for (int v = 0; v < m_nv; ++v)
            m_total_weight += m_vw[v];
        const int avg = m_total_weight / 2;
        const int slack = (avg * m_cfg.balance_pct + 99) / 100;
        m_max_weight = std::max(avg + slack, avg + 1);

        m_min_vw = m_nv > 0 ? *std::min_element(m_vw.begin(), m_vw.end()) : 0;

        // Gmax = max_v Σ 关联 net 权重（spec §4.4）
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
            // 平衡修复趟（false -> true）不计入收敛判断，见 spec §5.3
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
    // 固定点先落位，自由点按随机顺序贪心放到较轻一侧
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

    // 从 m_part 整体重建 cnt / wsum / cut
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

    // Task 3 整体替换本函数体（FM pass：增益桶、移动、前缀回滚）
    int run_one_pass()
    {
        return 0;
    }

    // ---- 图快照（构造后只读） ----
    Config m_cfg;
    int m_nv = 0, m_ne = 0;
    std::vector<int> m_vw, m_nw;
    std::vector<int> m_pin_start, m_pin_list;
    std::vector<int> m_net_start, m_net_list;
    int m_total_weight = 0;
    int m_max_weight = 0;
    int m_min_vw = 0;
    int m_gmax = 0;

    // ---- 每次 run() 重置的 FM 状态 ----
    std::vector<uint8_t> m_part;
    std::vector<int8_t> m_fixed;
    std::vector<int> m_cnt;              // 扁平 [2*e + side]
    std::vector<char> m_locked;
    int m_wsum[2] = {0, 0};
    int m_cut = 0;
    std::vector<int> m_trail;
    GainBuckets m_buckets;
    int m_check_failures = 0;
};

} // namespace fox::fmpart

#endif // FMPART_FMPART_HPP
