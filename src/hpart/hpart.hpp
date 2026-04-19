#ifndef HPART_HPP
#define HPART_HPP

#include "misc/util/abc_global.h"
#include "base/main/main.h"

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
};

const char *ToolName(Tool tool);
bool ApplyPartitioning(Abc_Ntk_t *pNtk, const Config &cfg);

} // namespace fox::hpart

#endif // HPART_HPP
