#include "csr_internal.hpp"

#include "base/abc/abcPdb.hpp"
#include "cpr/cpr.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace fox::csr::detail {

bool GrowthTracker::TryConsume(int positive_net_growth)
{
    if (positive_net_growth <= 0)
        return true;
    if (static_cast<int64_t>(used_) + positive_net_growth > budget_)
        return false;
    used_ += positive_net_growth;
    return true;
}

bool SearchBudget::TryBeamState()
{
    if (beam_states >= kMaxBeamStatesPerRound)
        return false;
    ++beam_states;
    return true;
}

bool SearchBudget::TryDivisorSet()
{
    if (divisor_sets >= kMaxDivisorSetsPerNode)
        return false;
    ++divisor_sets;
    return true;
}

bool SearchBudget::TrySatCall()
{
    if (sat_calls >= kMaxSatCallsPerNode)
        return false;
    ++sat_calls;
    return true;
}

bool SearchBudget::TrySuccessfulPlan()
{
    if (successful_plans >= kMaxSuccessfulPlansPerNode)
        return false;
    ++successful_plans;
    return true;
}

bool SearchBudget::TryCluster()
{
    if (clusters >= kMaxClustersPerDriverPart)
        return false;
    ++clusters;
    return true;
}

OptimizationState::OptimizationState(Abc_Ntk_t *pNetwork,
                                     const EntryLimits &entry_limits,
                                     int tid)
    : pNtk(pNetwork)
    , limits(entry_limits)
    , entry(ComputeMetrics(pNetwork))
    , current(ComputeMetrics(pNetwork))
    , growth(entry_limits.growth_budget)
    , trajectory_id(tid)
{
    fox::cpr::partition_sizes(pNtk, limits.num_parts, part_sizes);
}

void OptimizationState::AttachNetwork(Abc_Ntk_t *pNetwork)
{
    pNtk = pNetwork;
    current = ComputeMetrics(pNtk);
    fox::cpr::partition_sizes(pNtk, limits.num_parts, part_sizes);
}

bool OptimizationState::Audit()
{
    current = ComputeMetrics(pNtk);
    return current.hop <= limits.hop_limit
        && current.nodes <= limits.node_limit
        && current.cut_nets <= limits.cutnet_limit
        && growth.used() <= limits.growth_budget;
}

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
