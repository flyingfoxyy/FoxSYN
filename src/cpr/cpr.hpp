#ifndef CPR_HPP
#define CPR_HPP

#include "misc/util/abc_global.h"
#include "base/main/main.h"

namespace fox::cpr {

struct Config {
    int balance_pct           = -1;   // -1 = inherit from pdb (falls back to 2 if pdb has none)
    int replicate_growth_pct  = 2;    // max % of original node count that replicate may add
    int cutsize_growth_pct    = 25;   // max % cut-net growth across the whole cpr run
    int relocate_max_rounds   = 100;  // hard cap on relocate sweeps
    int relocate_stall_limit  = 20;   // quit relocate after N consecutive rounds with no timing gain
    int replicate_max_rounds  = 50;   // hard cap on replicate sweeps
    int replicate_stall_limit = 10;   // quit replicate after N consecutive rounds with no timing gain
    bool verbose              = false;
};

bool ApplyCpr(Abc_Ntk_t *pNtk, const Config &cfg);

} // namespace fox::cpr

#endif // CPR_HPP
