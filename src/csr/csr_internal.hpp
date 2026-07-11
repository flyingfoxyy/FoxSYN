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

class GrowthTracker {
public:
    explicit GrowthTracker(int budget) : budget_(budget) {}
    bool TryConsume(int positive_net_growth);
    void RecordDeletion(int) {}
    int used() const { return used_; }
    int remaining() const { return budget_ - used_; }

private:
    int budget_ = 0;
    int used_ = 0;
};

struct SearchBudget {
    static constexpr int kMaxBeamStatesPerRound = 512;
    static constexpr int kMaxDivisorSetsPerNode = 32;
    static constexpr int kMaxSatCallsPerNode = 128;
    static constexpr int kMaxSuccessfulPlansPerNode = 4;
    static constexpr int kMaxClustersPerDriverPart = 16;

    int beam_states = 0;
    int divisor_sets = 0;
    int sat_calls = 0;
    int successful_plans = 0;
    int clusters = 0;

    bool TryBeamState();
    bool TryDivisorSet();
    bool TrySatCall();
    bool TrySuccessfulPlan();
    bool TryCluster();
};

struct OptimizationState {
    Abc_Ntk_t *pNtk = nullptr;
    EntryLimits limits;
    Metrics entry;
    Metrics current;
    GrowthTracker growth;
    int trajectory_id = 0;
    std::vector<int> part_sizes;

    OptimizationState(Abc_Ntk_t *pNetwork, const EntryLimits &entry_limits, int tid);
    void AttachNetwork(Abc_Ntk_t *pNetwork);
    bool Audit();
};

Metrics ComputeMetrics(Abc_Ntk_t *pNtk);
int ComputePercentageLimit(int count, int percentage, bool round_up);
EntryLimits CaptureEntryLimits(Abc_Ntk_t *pNtk, const Config &cfg);
void RestorePdbMetadata(Abc_Ntk_t *pNtk, const EntryLimits &limits);

} // namespace fox::csr::detail

#endif
