#include "csr_internal.hpp"

#include "base/abc/abcPdb.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace fox::csr::detail {

Metrics ComputeMetrics(Abc_Ntk_t *pNtk)
{
    return {
        ComputeCutEdgeCount(pNtk),
        Abc_NtkComputeCutSize(pNtk),
        Abc_NtkComputeHopNum(pNtk),
        Abc_NtkNodeNum(pNtk),
    };
}

int ComputePercentageLimit(int count, int percentage, bool round_up)
{
    if (count <= 0 || percentage <= 0)
        return 0;
    const int64_t scaled = static_cast<int64_t>(count) * percentage;
    const int64_t limit = (scaled + (round_up ? 99 : 0)) / 100;
    return limit > std::numeric_limits<int>::max()
        ? std::numeric_limits<int>::max()
        : static_cast<int>(limit);
}

EntryLimits CaptureEntryLimits(Abc_Ntk_t *pNtk, const Config &cfg)
{
    Metrics metrics = ComputeMetrics(pNtk);
    int num_parts = pNtk->pPdb->num_parts();
    if (num_parts <= 0)
    {
        part_id max_part = 0;
        Abc_Obj_t *pObj;
        int i;
        Abc_NtkForEachObj(pNtk, pObj, i)
            if (Abc_ObjGetPartId(pObj) != ABC_PART_ID_NONE)
                max_part = std::max(max_part, Abc_ObjGetPartId(pObj));
        num_parts = static_cast<int>(max_part) + 1;
    }
    int balance_pct = cfg.balance_pct >= 0 ? cfg.balance_pct : pNtk->pPdb->balance_pct();
    if (balance_pct < 0)
        balance_pct = 2;
    const int growth_budget = ComputePercentageLimit(
        metrics.nodes, cfg.replicate_growth_pct, false);
    return {
        num_parts,
        balance_pct,
        metrics.hop,
        ComputePercentageLimit(metrics.nodes, 102, true),
        growth_budget,
        ComputePercentageLimit(metrics.cut_nets, 150, true),
    };
}

void RestorePdbMetadata(Abc_Ntk_t *pNtk, const EntryLimits &limits)
{
    const Metrics metrics = ComputeMetrics(pNtk);
    Abc_NtkSetPartStats(pNtk, limits.num_parts, metrics.cut_nets, metrics.hop);
    pNtk->pPdb->set_balance_pct(limits.balance_pct);
}

} // namespace fox::csr::detail
