#ifndef PDECOMP_HPP
#define PDECOMP_HPP

#include "misc/util/abc_global.h"
#include "base/main/main.h"

namespace fox::pdecomp {

struct Config {
    int K = 2; // -K: target max fanin count per node after decomposition (2-6)
};

bool ApplyPdecomp(Abc_Frame_t *pAbc, const Config &cfg);

} // namespace fox::pdecomp

#endif // PDECOMP_HPP
