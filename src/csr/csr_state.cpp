#include "csr_internal.hpp"

#include "base/abc/abcPdb.hpp"

#include <algorithm>

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
    const int growth_budget = static_cast<int>(
        static_cast<long long>(metrics.nodes) * cfg.replicate_growth_pct / 100);
    return {
        num_parts,
        balance_pct,
        metrics.hop,
        (metrics.nodes * 102 + 99) / 100,
        growth_budget,
        (metrics.cut_nets * 150 + 99) / 100,
    };
}

void RestorePdbMetadata(Abc_Ntk_t *pNtk, const EntryLimits &limits)
{
    const Metrics metrics = ComputeMetrics(pNtk);
    Abc_NtkSetPartStats(pNtk, limits.num_parts, metrics.cut_nets, metrics.hop);
    pNtk->pPdb->set_balance_pct(limits.balance_pct);
}

} // namespace fox::csr::detail
