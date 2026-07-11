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

enum class TrajectoryPolicy {
    GainFirst = 0,
    BoundaryConcentration = 1,
    ScarcityFirst = 2,
};

struct CutCandidate {
    int node_id = -1;
    int iFanin = -1;
    int weight = 0;
    int target_options = 0;
};

struct CutCandidateLess {
    bool operator()(const CutCandidate &lhs, const CutCandidate &rhs) const
    {
        return std::tuple{-lhs.weight, lhs.node_id, lhs.iFanin}
             < std::tuple{-rhs.weight, rhs.node_id, rhs.iFanin};
    }
};

struct ReplicationKey {
    int driver_id = -1;
    part_id target_part = ABC_PART_ID_NONE;
};

struct ReplicationCandidate {
    ReplicationKey key;
    int saved_edges = 0;
    int added_edges = 0;
    int cutnet_delta = 0;
    int node_cost = 1;
    int net_gain() const { return saved_edges - added_edges; }
};

std::vector<ReplicationCandidate> CollectReplicationCandidates(Abc_Ntk_t *pNtk);

struct ReplicationCluster {
    ReplicationKey key;
    std::vector<int> node_ids;
    int cutedge_delta = 0;
    int cutnet_delta = 0;
    int positive_net_growth = 0;
};

struct OptimizationState;
class HopState;

ReplicationCluster FindBestReplicationCluster(
    Abc_Ntk_t *pNtk, OptimizationState &state,
    const ReplicationCandidate &candidate, HopState &hop);
bool TryReplicationCluster(Abc_Ntk_t *pNtk, OptimizationState &state,
                           HopState &hop, const ReplicationCluster &cluster);

struct TrajectoryResult {
    Abc_Ntk_t *pNtk = nullptr;
    Metrics metrics;
    int trajectory_id = 0;
    bool valid = false;
};

struct EntryLimits;
bool BetterResult(const TrajectoryResult &lhs, const TrajectoryResult &rhs);
using NetworkDeleteFn = void (*)(Abc_Ntk_t *);
TrajectoryResult TakeBestTrajectory(std::vector<TrajectoryResult> &results,
                                    const EntryLimits &limits,
                                    NetworkDeleteFn delete_fn);

struct EntryLimits {
    int num_parts = 0;
    int balance_pct = 2;
    int hop_limit = 0;
    int node_limit = 0;
    int growth_budget = 0;
    int cutnet_limit = 0;
    // Balance overflow already present in the entry partition, under
    // balance_pct's formula. hmetis's own partitions routinely exceed the
    // nominal tolerance (recursive bisection compounds error across levels),
    // so the audit gate must not regress past the entry, not enforce zero
    // overflow outright -- mirrors hop_limit/node_limit/cutnet_limit, which
    // are all measured from the entry state rather than fixed absolutes.
    int balance_overflow_limit = 0;
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

class HopState {
public:
    struct Change { int obj_id; int old_arrival; };
    struct Transaction {
        std::vector<Change> changes;
        std::vector<char> logged;
    };

    bool Initialize(Abc_Ntk_t *pNtk);
    Transaction BeginTransaction() const;
    bool PropagateFrom(Abc_Ntk_t *pNtk, const std::vector<int> &start_ids,
                       int hop_limit, Transaction &txn);
    void Rollback(Transaction &txn);
    bool VerifyAgainstFull(Abc_Ntk_t *pNtk) const;
    int arrival(int obj_id) const;
    int topo_rank(int obj_id) const;

private:
    std::vector<int> arrival_;
    std::vector<int> topo_rank_;
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

struct DivisorInfo {
    int obj_id = -1;
    part_id part = ABC_PART_ID_NONE;
    int cex_coverage = 0;
    int predicted_cutedge_delta = 0;
    int predicted_cutnet_delta = 0;
    int hop_arrival = 0;
};

struct DivisorInfoLess {
    bool operator()(const DivisorInfo &a, const DivisorInfo &b) const
    {
        return std::tuple{-a.cex_coverage, a.predicted_cutedge_delta,
                          a.predicted_cutnet_delta, a.hop_arrival, a.obj_id}
             < std::tuple{-b.cex_coverage, b.predicted_cutedge_delta,
                          b.predicted_cutnet_delta, b.hop_arrival, b.obj_id};
    }
};

int ComputeHypotheticalCutNetDelta(
    Abc_Obj_t *pConsumer,
    const std::vector<Abc_Obj_t *> &old_fanins,
    const std::vector<Abc_Obj_t *> &new_fanins);

struct ResubPlan {
    std::vector<int> removed_fanin_indices;
    std::vector<int> divisor_ids;
    Hop_Obj_t *pFunc = nullptr;
    int cutedge_delta = 0;
    int cutnet_delta = 0;
    int predicted_hop = 0;
    int new_fanin_count = 0;
    int positive_net_growth = 0;
};

std::optional<ResubPlan> SelectBestResubPlan(
    const std::vector<ResubPlan> &plans);
bool ExternalDivisorPlanAllowed(int removed_crossings, int added_crossings);
bool ResubPlanAllowed(const ResubPlan &plan, const OptimizationState &state);

struct Phase1Stats {
    int attempts = 0;
    int successes = 0;
    int single_removals = 0;
    int joint_removals = 0;
    int joint_replacements = 0;
    int multi_divisor = 0;
};

bool RunPhase1Resub(Abc_Ntk_t *pNtk, OptimizationState &state,
                    const Config &cfg, Phase1Stats &stats);

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

struct RelocationStep {
    int node_id = -1;
    part_id from = ABC_PART_ID_NONE;
    part_id to = ABC_PART_ID_NONE;
};

struct RelocationSequence {
    std::vector<RelocationStep> steps;
    int cutedge_delta = 0;
};

RelocationSequence FindBestRelocationSequence(
    Abc_Ntk_t *pNtk, OptimizationState &state, TrajectoryPolicy policy);
bool ApplyRelocationSequence(Abc_Ntk_t *pNtk, OptimizationState &state,
                             const RelocationSequence &sequence);

Metrics ComputeMetrics(Abc_Ntk_t *pNtk);
int ComputePercentageLimit(int count, int percentage, bool round_up);
EntryLimits CaptureEntryLimits(Abc_Ntk_t *pNtk, const Config &cfg);
void RestorePdbMetadata(Abc_Ntk_t *pNtk, const EntryLimits &limits);

} // namespace fox::csr::detail

#endif
