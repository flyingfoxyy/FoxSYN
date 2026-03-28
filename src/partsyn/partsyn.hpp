#ifndef PARTSYN_HPP
#define PARTSYN_HPP

#include "misc/util/abc_global.h"
#include "base/main/main.h"

namespace fox::partsyn {

struct Config {
    int num_parts = 4;      // Number of partitions
    bool verbose = false;   // Verbose output

    // Optimization parameters for each partition
    int opt_rounds = 3;     // Number of optimization rounds per partition
};

// Main entry point for partition-based parallel synthesis
Abc_Ntk_t* PerformPartSyn(Abc_Ntk_t* pNtk, const Config& cfg);

} // namespace fox::partsyn

#endif // PARTSYN_HPP
