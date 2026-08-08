#ifndef HIVE_CONVEX_HPP
#define HIVE_CONVEX_HPP

#include <vector>

#include "hive/hive_graph.hpp"
#include "hive/region.hpp"

namespace fox::hive {

struct ClosureResult {
    bool ok = false;              // false: budget exceeded, reject the move
    std::vector<int> violators;   // sorted; valid only when ok
};

// Violators of R ∪ added: vertices outside the set that lie on a path from
// the set back into the set. Rank-band pruned per docs/hive-design.md 3.3;
// `budget` caps marked vertices per direction.
ClosureResult ComputeClosure(const CombGraph &g, const Region &r,
                             const std::vector<int> &added, int budget);
// Independent brute-force convexity check: full forward/backward
// reachability, no rank pruning, no budget. Test/verification only.
bool IsConvexBrute(const CombGraph &g, const std::vector<int> &memberIds);

} // namespace fox::hive

#endif // HIVE_CONVEX_HPP