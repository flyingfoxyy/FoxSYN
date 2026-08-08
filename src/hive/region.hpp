#ifndef HIVE_REGION_HPP
#define HIVE_REGION_HPP

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "hive/hive_graph.hpp"

namespace fox::hive {

struct Metrics {
    int n = 0, in = 0, out = 0;
    double q = 0.0;   // n / (in + out); 0 when the boundary is empty
    int lb = 0, gap = 0;
    int rank_min = 0, rank_max = 0;
};

// Incrementally maintained region (docs/hive-design.md 2.2, 5). Uses only
// small hash containers keyed by object id, so copying a Region for a trial
// move is cheap; the CombGraph holds all id-indexed bulk data.
class Region {
public:
    explicit Region(const CombGraph &g) : m_g(&g) {}

    void init(const std::vector<int> &memberIds);
    void add(int id);
    bool contains(int id) const { return m_members.count(id) != 0; }
    int size() const { return (int)m_members.size(); }

    std::vector<int> member_ids() const;
    std::vector<int> in_objects() const;
    std::vector<int> entrance_candidates() const;
    std::vector<int> out_members() const;

    Metrics metrics(int lut_k) const;
    Metrics recompute(int lut_k) const;  // independent path, for cross-checks

private:
    int lower_bound_luts(int lut_k) const;  // Task 4; returns 0 until then

    const CombGraph *m_g;
    std::unordered_set<int> m_members;
    // external driver id -> number of members it drives (never contains members)
    std::unordered_map<int, int> m_driven_cnt;
    // member id -> number of vertex succs outside the region
    std::unordered_map<int, int> m_ext_succ;
    int m_in = 0;   // |m_driven_cnt| keys with cnt > 0 (all keys qualify)
    int m_out = 0;  // members with has_ext_out || ext_succ > 0
};

} // namespace fox::hive

#endif // HIVE_REGION_HPP
