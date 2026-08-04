#ifndef FMPART_FM_BUCKETS_HPP
#define FMPART_FM_BUCKETS_HPP

#include <cstdio>
#include <vector>

namespace fox::fmpart {

// FM 增益桶：每侧一个桶数组，桶内侵入式双向链表，max 指针懒下降。
// 除 find_top 的向下扫描外全部 O(1)（见 docs/fmpart-design.md §5.2.1）。
class GainBuckets {
public:
    static constexpr int kNone = -1;

    void reset(int num_vertices, int gmax)
    {
        m_gmax = gmax;
        m_width = 2 * gmax + 1;
        m_head.assign(2 * m_width, kNone);
        m_next.assign(num_vertices, kNone);
        m_prev.assign(num_vertices, kNone);
        m_gain.assign(num_vertices, 0);
        m_side.assign(num_vertices, 0);
        m_in.assign(num_vertices, 0);
        m_max[0] = m_max[1] = -m_gmax - 1;   // 低于一切合法增益 == 空
    }

    bool contains(int v) const { return m_in[v] != 0; }
    int  gain_of(int v) const { return m_gain[v]; }
    int  side_of(int v) const { return m_side[v]; }

    void insert(int v, int side, int gain)
    {
        // 调用方保证 !contains(v) 且 -gmax <= gain <= gmax
        const int b = bucket_index(side, gain);
        m_gain[v] = gain;
        m_side[v] = side;
        m_in[v] = 1;
        m_prev[v] = kNone;
        m_next[v] = m_head[b];
        if (m_head[b] != kNone)
            m_prev[m_head[b]] = v;
        m_head[b] = v;
        if (gain > m_max[side])
            m_max[side] = gain;
    }

    void erase(int v)
    {
        if (!m_in[v])
            return;
        const int b = bucket_index(m_side[v], m_gain[v]);
        if (m_prev[v] != kNone)
            m_next[m_prev[v]] = m_next[v];
        else
            m_head[b] = m_next[v];
        if (m_next[v] != kNone)
            m_prev[m_next[v]] = m_prev[v];
        m_next[v] = m_prev[v] = kNone;
        m_in[v] = 0;
        // m_max 允许暂时虚高，max_gain() 查询时懒下降
    }

    void update_gain(int v, int gain)
    {
        const int side = m_side[v];
        erase(v);
        insert(v, side, gain);
    }

    bool empty(int side) { return max_gain(side) < -m_gmax; }

    // 该侧当前最大增益；空侧返回 -gmax-1
    int max_gain(int side)
    {
        int g = m_max[side];
        while (g >= -m_gmax && m_head[bucket_index(side, g)] == kNone)
            --g;
        m_max[side] = g;
        return g;
    }

    // 从 side 顶端向下找第一个满足 feasible 的顶点，找不到返回 kNone。
    // 只经由 max_gain() 跳过空桶；不会把 max 指针降过仍有元素的桶。
    template <typename Pred>
    int find_top(int side, Pred feasible)
    {
        for (int g = max_gain(side); g >= -m_gmax; --g)
            for (int v = m_head[bucket_index(side, g)]; v != kNone; v = m_next[v])
                if (feasible(v))
                    return v;
        return kNone;
    }

    // 结构自检：返回不一致数（0 = 一致），逐条打印到 stderr
    int check_consistency()
    {
        int bad = 0;
        for (int side = 0; side < 2; ++side)
            for (int g = -m_gmax; g <= m_gmax; ++g)
                for (int v = m_head[bucket_index(side, g)]; v != kNone; v = m_next[v]) {
                    if (!m_in[v] || m_side[v] != side || m_gain[v] != g) {
                        std::fprintf(stderr, "GainBuckets: vertex %d in wrong bucket\n", v);
                        ++bad;
                    }
                    if (m_next[v] != kNone && m_prev[m_next[v]] != v) {
                        std::fprintf(stderr, "GainBuckets: broken links at %d\n", v);
                        ++bad;
                    }
                }
        for (int side = 0; side < 2; ++side) {
            int true_max = -m_gmax - 1;
            for (int g = m_gmax; g >= -m_gmax; --g)
                if (m_head[bucket_index(side, g)] != kNone) { true_max = g; break; }
            if (max_gain(side) != true_max) {
                std::fprintf(stderr, "GainBuckets: settled max != true max on side %d\n", side);
                ++bad;
            }
        }
        return bad;
    }

private:
    int bucket_index(int side, int gain) const { return side * m_width + (gain + m_gmax); }

    int m_gmax = 0;
    int m_width = 1;
    int m_max[2] = {0, 0};
    std::vector<int> m_head, m_next, m_prev, m_gain, m_side;
    std::vector<char> m_in;
};

} // namespace fox::fmpart

#endif // FMPART_FM_BUCKETS_HPP
