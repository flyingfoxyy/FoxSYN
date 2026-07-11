#ifndef CSR_INTERNAL_HPP
#define CSR_INTERNAL_HPP

#include <optional>
#include <tuple>
#include <vector>

#include "csr.hpp"
#include "base/abc/abc.h"

namespace fox::csr::detail {

struct Metrics {
    int cut_edges = 0;
    int cut_nets = 0;
    int hop = 0;
    int nodes = 0;
};

struct EntryLimits {
    int num_parts = 0;
    int balance_pct = 2;
    int hop_limit = 0;
    int node_limit = 0;
    int growth_budget = 0;
    int cutnet_limit = 0;
};

Metrics ComputeMetrics(Abc_Ntk_t *pNtk);
EntryLimits CaptureEntryLimits(Abc_Ntk_t *pNtk, const Config &cfg);
void RestorePdbMetadata(Abc_Ntk_t *pNtk, const EntryLimits &limits);

} // namespace fox::csr::detail

#endif
