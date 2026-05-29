#ifndef CMFS_HPP
#define CMFS_HPP

#include "misc/util/abc_global.h"
#include "base/main/main.h"

namespace fox::cmfs {

struct Config {
    int  top_K       = 16;
    int  max_rounds  = 20;
    int  stall_limit = 3;
    int  nBTLimit    = 5000;
    int  nWinTfoLevs = 2;
    int  nFanoutsMax = 30;
    int  nWinMax     = 300;
    int  maxTempLut  = 0;   // -X: max temp LUT size for Shannon decomp (0=off, 7-12)
    bool allow_resub = false;
    bool verbose     = false;
};

bool ApplyCmfs(Abc_Ntk_t *pNtk, const Config &cfg);

} // namespace fox::cmfs

#endif // CMFS_HPP
