#ifndef CSR3_HPP
#define CSR3_HPP

#include "misc/util/abc_global.h"
#include "base/main/main.h"

namespace fox::csr3 {

struct Config {
    int  jaccard_pct = 30;      // -J: Jaccard grouping threshold, percent (1-99)
    int  max_lines   = 16;      // -M: max lines per group (k cap)
    int  sim_words   = 16;      // -P: random-sim words (x64 patterns each)
    int  btlimit     = 100000;  // -B: SAT backtrack limit per solve
    bool self_check  = false;   // -c: exhaustive cross-check for |support|<=16 groups
    bool verbose     = false;   // -v
};

bool RunCsr3(Abc_Ntk_t *pNtk, const Config &cfg);

} // namespace fox::csr3
#endif // CSR3_HPP
