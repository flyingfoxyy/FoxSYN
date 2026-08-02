#ifndef CSR4_HPP
#define CSR4_HPP

#include "misc/util/abc_global.h"
#include "base/main/main.h"

namespace fox::csr4 {

struct Config {
    int  lut_size  = 6;      // -K: LUT input cap used for the feasibility check
    int  max_bound = 12;     // -B: max |B_kill| per group (enumeration is 2^|B_kill|)
    int  max_luts  = 8;      // -L: max consumer LUTs per group
    bool apply     = false;  // -a: Phase 1 -- actually rewrite the netlist
    bool verbose   = false;  // -v: per-group detail
};

bool RunCsr4(Abc_Ntk_t *pNtk, const Config &cfg);

} // namespace fox::csr4
#endif // CSR4_HPP
