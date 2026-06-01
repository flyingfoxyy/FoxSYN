#ifndef HPART_HPP
#define HPART_HPP

#include "misc/util/abc_global.h"
#include "base/main/main.h"
#include <string>

namespace fox::hpart {

enum class Tool {
    HMetis,
    SHMetis,
    KMetis,
};

struct Config {
    Tool tool = Tool::HMetis;
    int num_parts = 4;
    int balance_pct = 2;
    bool verbose = false;
    std::string save_part;  // if non-empty, save partition result to this file
    std::string load_part;  // if non-empty, load partition result from this file (skip hmetis)
};

const char *ToolName(Tool tool);
bool ApplyPartitioning(Abc_Ntk_t *pNtk, const Config &cfg);

} // namespace fox::hpart

#endif // HPART_HPP
