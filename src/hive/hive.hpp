#ifndef HIVE_HPP
#define HIVE_HPP

#include <vector>

#include "misc/util/abc_global.h"
#include "base/main/main.h"

namespace fox::hive {

struct Config {
    int num_regions = 20;   // -N: regions to report (>=1)
    int max_nodes   = 64;   // -M: region node cap (>=1)
    int max_in      = 32;   // -I: input cap Imax (>=1)
    int max_out     = 8;    // -O: output cap Omax (>=1)
    int lut_k       = 6;    // -K: LUT width, LB computation only (2-16)
    int num_seeds   = 2000; // -S: seed cap, 0 = all nodes (>=0)
    bool verbose    = false;// -v
};

// Internal constants (docs/hive-design.md 6). Exposed so tests can reference
// the exact values they pin as regression baselines.
inline constexpr int kTopL          = 4;    // exact evaluations per growth step
inline constexpr int kStall         = 8;    // steps without a new best before stop
inline constexpr int kJaccardPct    = 50;   // near-duplicate threshold, percent
inline constexpr int kClosureBudget = 2048; // visited-vertex cap per closure scan
inline constexpr int kFanoutCap     = 16;   // exit-candidate samples per out member

struct RegionReport {
    int root_id = 0;              // seed root object id
    std::vector<int> member_ids;  // sorted ascending
    int n = 0, in = 0, out = 0;
    int lb = 0, gap = 0;
    int rank_min = 0, rank_max = 0;
    double q = 0.0;
};

struct Result {
    bool ok = false;                    // false: preconditions failed
    std::vector<RegionReport> regions;  // Q-descending, Jaccard-deduped
};

Result RunHive(Abc_Ntk_t *pNtk, const Config &cfg);   // core, unit-testable
bool ApplyHive(Abc_Frame_t *pAbc, const Config &cfg); // checks + RunHive + report

} // namespace fox::hive

#endif // HIVE_HPP
