#ifndef CSR_HPP
#define CSR_HPP

#include "misc/util/abc_global.h"
#include "base/main/main.h"

namespace fox::csr {

struct Config {
    int  max_rounds           = 20;   // per-phase round cap (Phase 1 and Phase 2 each use this independently)
    int  stall_limit          = 3;    // per-phase stall limit
    int  nBTLimit              = 5000;
    int  nWinTfoLevs           = 2;
    int  nFanoutsMax           = 30;
    int  nWinMax                = 300;
    int  maxTempLut            = 0;   // -X: Shannon decomp max temp LUT size (0=off, 7-12), Phase 1 only
    int  replicate_growth_pct = 2;    // -G: Phase 2 node growth cap, % of original node count
    int  balance_pct          = -1;   // -B: -1 = inherit from pdb (falls back to 2 like cpr)
    bool do_balance_repair    = false; // -b: run cpr-style enforce_balance after phase1/2 (off by default)
    bool verbose              = false;
};

bool ApplyCsr(Abc_Ntk_t *pNtk, const Config &cfg);

// Cut-edge count: number of (driver, consumer) pairs whose partitions
// differ. Distinct from Abc_NtkComputeCutSize()'s cut-NET count, which
// counts each driver at most once regardless of how many partitions its
// fanouts span.
int ComputeCutEdgeCount(Abc_Ntk_t *pNtk);

} // namespace fox::csr

#endif // CSR_HPP
