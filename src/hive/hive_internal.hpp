#ifndef HIVE_INTERNAL_HPP
#define HIVE_INTERNAL_HPP

#include "hive/hive.hpp"
#include "hive/hive_graph.hpp"

namespace fox::hive {

struct GrowResult {
    bool has_region = false;   // false: seed skipped (over caps) or invalid root
    RegionReport report;
};

// Grows one region from the MFFC of rootId (docs/hive-design.md 4).
GrowResult GrowFromSeed(const CombGraph &g, int rootId, const Config &cfg);

} // namespace fox::hive

#endif // HIVE_INTERNAL_HPP
