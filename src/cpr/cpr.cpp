#include "cpr.hpp"

#include "timer/timer.hpp"

#include "base/abc/abc.h"
#include "base/abc/abcPdb.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <map>
#include <utility>
#include <vector>

namespace fox::cpr {

using ::fox::timer::SimpleTimer;

static constexpr float EPS = 1e-3f;
static constexpr int   DEFAULT_BALANCE_PCT = 2;

// Per-replicate bookkeeping so a round can be rolled back if it breaches the
// cut-net cap. Each entry is one accepted duplication within try_replicate.
struct ReplicateEntry {
    Abc_Obj_t *pObj;
    Abc_Obj_t *pDup;
    std::vector<Abc_Obj_t *> patched_fanouts;
};

static int get_num_parts(Abc_Ntk_t *pNtk)
{
    if (!pNtk->pPdb)
        return 0;
    int np = pNtk->pPdb->num_parts();
    if (np > 0)
        return np;

    // Fallback: scan all objects for max part id
    int max_part = 0;
    int i;
    Abc_Obj_t *pObj;
    Abc_NtkForEachObj(pNtk, pObj, i)
    {
        part_id p = Abc_ObjGetPartId(pObj);
        if (p != ABC_PART_ID_NONE && p > max_part)
            max_part = p;
    }
    return max_part + 1;
}

// A node is "near a hop boundary" if at least one of its fanins or fanouts
// already lives in a different partition. Only these are useful relocate
// candidates; moving an interior node can only create new hops.
static bool is_near_hop_boundary(Abc_Obj_t *pObj)
{
    part_id my = Abc_ObjGetPartId(pObj);
    if (my == ABC_PART_ID_NONE)
        return false;

    Abc_Obj_t *pOther;
    int i;
    Abc_ObjForEachFanin(pObj, pOther, i)
    {
        part_id p = Abc_ObjGetPartId(pOther);
        if (p != ABC_PART_ID_NONE && p != my)
            return true;
    }
    Abc_ObjForEachFanout(pObj, pOther, i)
    {
        part_id p = Abc_ObjGetPartId(pOther);
        if (p != ABC_PART_ID_NONE && p != my)
            return true;
    }
    return false;
}

static bool try_relocate(Abc_Obj_t *pObj, SimpleTimer &timer, int num_parts)
{
    part_id old_part = Abc_ObjGetPartId(pObj);
    std::vector<float> saved = timer.get_arrival();
    float base_max = timer.max_arrival();
    part_id best_part = old_part;
    float best_max = base_max;

    for (int p = 0; p < num_parts; ++p)
    {
        part_id cand = static_cast<part_id>(p);
        if (cand == old_part)
            continue;
        Abc_ObjSetPartId(pObj, cand);
        timer.recompute_cone(pObj);
        float cur_max = timer.max_arrival();
        if (cur_max < best_max)
        {
            best_max = cur_max;
            best_part = cand;
        }
        // Restore for next trial
        timer.set_arrival(saved);
    }

    // Apply best partition found
    Abc_ObjSetPartId(pObj, best_part);
    if (best_part != old_part)
    {
        timer.recompute_cone(pObj);
        return true;
    }
    return false;
}

static Abc_Obj_t *duplicate_node(Abc_Ntk_t *pNtk, Abc_Obj_t *pObj)
{
    // Abc_NtkDupObj copies the object (including its mapped function / SOP)
    // but does NOT wire up fanins, so do that ourselves.
    Abc_Obj_t *pDup = Abc_NtkDupObj(pNtk, pObj, 0);
    if (!pDup)
        return nullptr;
    Abc_Obj_t *pFanin;
    int i;
    Abc_ObjForEachFanin(pObj, pFanin, i)
        Abc_ObjAddFanin(pDup, pFanin);
    return pDup;
}

// Returns how many duplicates were added (0 if none improved or budget hit).
// Each accepted duplicate is appended to `pLedger` (if non-null) so the
// caller can roll the whole round back on a cut-net cap breach.
static int try_replicate(Abc_Obj_t *pObj, SimpleTimer &timer, Abc_Ntk_t *pNtk, int budget,
                         std::vector<ReplicateEntry> *pLedger)
{
    if (budget <= 0)
        return 0;
    if (!Abc_ObjIsCutNet(pObj))
        return 0;

    // Group fanouts by their partition
    std::map<part_id, std::vector<Abc_Obj_t *>> fanouts_by_part;
    Abc_Obj_t *pFanout;
    int i;
    Abc_ObjForEachFanout(pObj, pFanout, i)
    {
        part_id p = Abc_ObjGetPartId(pFanout);
        if (p == ABC_PART_ID_NONE)
            continue;
        fanouts_by_part[p].push_back(pFanout);
    }

    part_id my_part = Abc_ObjGetPartId(pObj);
    int accepted = 0;
    std::vector<float> saved_arrival = timer.get_arrival();
    float base_max = timer.max_arrival();

    for (auto &kv : fanouts_by_part)
    {
        if (accepted >= budget)
            break;

        part_id target_part = kv.first;
        if (target_part == my_part)
            continue;

        const std::vector<Abc_Obj_t *> &fanouts = kv.second;
        if (fanouts.empty())
            continue;

        Abc_Obj_t *pDup = duplicate_node(pNtk, pObj);
        if (!pDup)
            continue;
        Abc_ObjSetPartId(pDup, target_part);

        for (Abc_Obj_t *pF : fanouts)
            Abc_ObjPatchFanin(pF, pObj, pDup);

        timer.recompute_cone(pDup);
        float new_max = timer.max_arrival();

        if (new_max < base_max)
        {
            base_max = new_max;
            saved_arrival = timer.get_arrival();
            accepted++;
            if (pLedger)
                pLedger->push_back({pObj, pDup, fanouts});
        }
        else
        {
            for (Abc_Obj_t *pF : fanouts)
                Abc_ObjPatchFanin(pF, pDup, pObj);
            Abc_NtkDeleteObj(pDup);
            timer.set_arrival(saved_arrival);
        }
    }
    return accepted;
}

// Count nodes in each partition, using the same accounting as hpart
// (every object with a valid part_id).
static void partition_sizes(Abc_Ntk_t *pNtk, int num_parts, std::vector<int> &sz)
{
    sz.assign(num_parts, 0);
    int i;
    Abc_Obj_t *pObj;
    Abc_NtkForEachObj(pNtk, pObj, i)
    {
        part_id p = Abc_ObjGetPartId(pObj);
        if (p == ABC_PART_ID_NONE)
            continue;
        if (static_cast<int>(p) < num_parts)
            sz[p] += 1;
    }
}

// Greedy repair that drives every partition size below `max_allowed`.
// Preference order when picking a node to move:
//   1. lowest-arrival node that already touches the target partition
//   2. lowest-arrival node in the overfull partition
// Returns true iff the constraint is satisfied on exit.
static bool enforce_balance(Abc_Ntk_t *pNtk, int num_parts, int balance_pct,
                            const std::vector<float> &arrival, bool verbose)
{
    std::vector<int> sz;
    partition_sizes(pNtk, num_parts, sz);
    int total = 0;
    for (int s : sz) total += s;
    if (total == 0 || num_parts <= 0)
        return true;

    int avg = total / num_parts;
    int slack = (avg * balance_pct + 99) / 100;
    int max_allowed = std::max(avg + slack, avg + 1);

    // Bucket nodes per partition so we can pick from overfull ones quickly.
    std::vector<std::vector<Abc_Obj_t *>> nodes_of(num_parts);
    int i;
    Abc_Obj_t *pObj;
    Abc_NtkForEachNode(pNtk, pObj, i)
    {
        part_id p = Abc_ObjGetPartId(pObj);
        if (p == ABC_PART_ID_NONE || static_cast<int>(p) >= num_parts)
            continue;
        nodes_of[p].push_back(pObj);
    }

    auto arr_of = [&arrival](Abc_Obj_t *p) {
        return (static_cast<size_t>(p->Id) < arrival.size()) ? arrival[p->Id] : 0.0f;
    };

    int moves = 0;
    const int guard = total * 2 + 8;
    for (int step = 0; step < guard; ++step)
    {
        int over = -1;
        for (int p = 0; p < num_parts; ++p)
            if (sz[p] > max_allowed) { over = p; break; }
        if (over < 0)
            break;

        int under = 0;
        for (int p = 1; p < num_parts; ++p)
            if (sz[p] < sz[under]) under = p;
        if (sz[under] >= max_allowed)
            break; // no room anywhere

        // Re-sort the overfull partition's candidates by ascending arrival so
        // we move the least-critical nodes first.
        std::sort(nodes_of[over].begin(), nodes_of[over].end(),
                  [&arr_of](Abc_Obj_t *a, Abc_Obj_t *b) {
                      return arr_of(a) < arr_of(b);
                  });

        Abc_Obj_t *pMove = nullptr;
        for (Abc_Obj_t *p : nodes_of[over])
        {
            Abc_Obj_t *q;
            int k;
            bool touches = false;
            Abc_ObjForEachFanin(p, q, k)
            {
                if (Abc_ObjGetPartId(q) == static_cast<part_id>(under))
                { touches = true; break; }
            }
            if (!touches)
            {
                Abc_ObjForEachFanout(p, q, k)
                {
                    if (Abc_ObjGetPartId(q) == static_cast<part_id>(under))
                    { touches = true; break; }
                }
            }
            if (touches) { pMove = p; break; }
        }
        if (!pMove && !nodes_of[over].empty())
            pMove = nodes_of[over].front(); // lowest arrival
        if (!pMove)
            break;

        Abc_ObjSetPartId(pMove, static_cast<part_id>(under));
        auto it = std::find(nodes_of[over].begin(), nodes_of[over].end(), pMove);
        if (it != nodes_of[over].end())
            nodes_of[over].erase(it);
        nodes_of[under].push_back(pMove);
        sz[over]--;
        sz[under]++;
        moves++;
    }

    bool ok = true;
    for (int p = 0; p < num_parts; ++p)
        if (sz[p] > max_allowed) { ok = false; break; }

    if (verbose)
    {
        printf("cpr: balance repair moved %d node(s) (max_allowed=%d, %s)\n",
               moves, max_allowed, ok ? "ok" : "still violated");
    }
    return ok;
}

static void revert_relocate_round(Abc_Ntk_t *pNtk,
                                  const std::vector<std::pair<Abc_Obj_t *, part_id>> &moved)
{
    for (auto it = moved.rbegin(); it != moved.rend(); ++it)
        Abc_ObjSetPartId(it->first, it->second);
    Abc_NtkUpdateCutNets(pNtk);
}

static void revert_replicate_round(Abc_Ntk_t *pNtk, std::vector<ReplicateEntry> &entries)
{
    for (auto it = entries.rbegin(); it != entries.rend(); ++it)
    {
        for (Abc_Obj_t *pF : it->patched_fanouts)
            Abc_ObjPatchFanin(pF, it->pDup, it->pObj);
        Abc_NtkDeleteObj(it->pDup);
    }
    entries.clear();
    Abc_NtkUpdateCutNets(pNtk);
}

static void run_relocate_phase(Abc_Ntk_t *pNtk, SimpleTimer &timer, int num_parts,
                               const Config &cfg, int max_allowed_cutsize)
{
    int stall = 0;
    for (int round = 0; round < cfg.relocate_max_rounds; ++round)
    {
        timer.compute_arrival();
        float prev_max = timer.max_arrival();

        std::vector<Abc_Obj_t *> cpath;
        timer.extract_critical_path(cpath);
        if (cpath.empty())
            break;

        std::vector<std::pair<Abc_Obj_t *, part_id>> moved;
        for (Abc_Obj_t *pObj : cpath)
        {
            if (!is_near_hop_boundary(pObj))
                continue;
            part_id before = Abc_ObjGetPartId(pObj);
            if (try_relocate(pObj, timer, num_parts) && Abc_ObjGetPartId(pObj) != before)
                moved.emplace_back(pObj, before);
        }

        Abc_NtkUpdateCutNets(pNtk);
        int cur_cut = Abc_NtkComputeCutSize(pNtk);
        if (cur_cut > max_allowed_cutsize)
        {
            revert_relocate_round(pNtk, moved);
            timer.compute_arrival();
            if (cfg.verbose)
                printf("cpr: relocate round %d breached cut cap (%d>%d); reverted, stopping phase\n",
                       round, cur_cut, max_allowed_cutsize);
            break;
        }

        timer.compute_arrival();
        float cur_max = timer.max_arrival();
        if (cfg.verbose)
            printf("cpr: relocate round %d  moves=%zu  max=%.2f  cut=%d/%d\n",
                   round, moved.size(), cur_max, cur_cut, max_allowed_cutsize);

        if (cur_max < prev_max - EPS)
        {
            stall = 0;
        }
        else
        {
            if (++stall >= cfg.relocate_stall_limit)
            {
                if (cfg.verbose)
                    printf("cpr: relocate stalled for %d rounds, stopping phase\n", stall);
                break;
            }
        }
    }
}

static void run_replicate_phase(Abc_Ntk_t *pNtk, SimpleTimer &timer, const Config &cfg,
                                int max_allowed_cutsize, int &extra_nodes, int max_extra_nodes)
{
    int stall = 0;
    for (int round = 0; round < cfg.replicate_max_rounds; ++round)
    {
        int remaining = max_extra_nodes - extra_nodes;
        if (remaining <= 0)
        {
            if (cfg.verbose)
                printf("cpr: replicate node budget exhausted, stopping phase\n");
            break;
        }

        timer.compute_arrival();
        float prev_max = timer.max_arrival();

        std::vector<Abc_Obj_t *> cpath;
        timer.extract_critical_path(cpath);
        if (cpath.empty())
            break;

        std::vector<ReplicateEntry> entries;
        int round_adds = 0;
        for (Abc_Obj_t *pObj : cpath)
        {
            remaining = max_extra_nodes - extra_nodes;
            if (remaining <= 0)
                break;
            int added = try_replicate(pObj, timer, pNtk, remaining, &entries);
            if (added > 0)
            {
                extra_nodes += added;
                round_adds  += added;
            }
        }

        Abc_NtkUpdateCutNets(pNtk);
        int cur_cut = Abc_NtkComputeCutSize(pNtk);
        if (cur_cut > max_allowed_cutsize)
        {
            revert_replicate_round(pNtk, entries);
            extra_nodes -= round_adds;
            timer.compute_arrival();
            if (cfg.verbose)
                printf("cpr: replicate round %d breached cut cap (%d>%d); reverted, stopping phase\n",
                       round, cur_cut, max_allowed_cutsize);
            break;
        }

        timer.compute_arrival();
        float cur_max = timer.max_arrival();
        if (cfg.verbose)
            printf("cpr: replicate round %d  adds=%d  max=%.2f  cut=%d/%d  nodes=%d/%d\n",
                   round, round_adds, cur_max, cur_cut, max_allowed_cutsize,
                   extra_nodes, max_extra_nodes);

        if (cur_max < prev_max - EPS)
        {
            stall = 0;
        }
        else
        {
            if (++stall >= cfg.replicate_stall_limit)
            {
                if (cfg.verbose)
                    printf("cpr: replicate stalled for %d rounds, stopping phase\n", stall);
                break;
            }
        }
    }
}

bool ApplyCpr(Abc_Ntk_t *pNtk, const Config &cfg)
{
    if (!pNtk)
    {
        printf("cpr: network is null\n");
        return false;
    }

    if (!pNtk->pPdb)
    {
        printf("cpr: network has no partition database (run hpart first)\n");
        return false;
    }

    if (Abc_NtkIsStrash(pNtk))
    {
        printf("cpr: does not support AIG networks (run mapping first)\n");
        return false;
    }

    int num_parts = get_num_parts(pNtk);
    if (num_parts < 2)
    {
        printf("cpr: need at least 2 partitions to optimize hops\n");
        return false;
    }

    // Resolve balance target: -B override > pdb > default.
    int balance_pct = cfg.balance_pct;
    if (balance_pct < 0)
        balance_pct = pNtk->pPdb->balance_pct();
    if (balance_pct < 0)
        balance_pct = DEFAULT_BALANCE_PCT;

    // Replication growth cap, expressed as number of extra nodes we may add.
    const int initial_nodes = Abc_NtkNodeNum(pNtk);
    const int max_extra_nodes =
        static_cast<int>(static_cast<long long>(initial_nodes) * cfg.replicate_growth_pct / 100);
    int extra_nodes = 0;

    // Baseline cut-net count and absolute cap.
    Abc_NtkUpdateCutNets(pNtk);
    const int initial_cutsize = Abc_NtkComputeCutSize(pNtk);
    int max_allowed_cutsize = initial_cutsize < 0
        ? std::numeric_limits<int>::max()
        : static_cast<int>(static_cast<long long>(initial_cutsize)
                           * (100 + cfg.cutsize_growth_pct) / 100);
    if (initial_cutsize > max_allowed_cutsize)
        max_allowed_cutsize = initial_cutsize;

    if (cfg.verbose)
    {
        printf("cpr: balance_pct=%d  replicate_budget=%d (%d%% of %d)\n",
               balance_pct, max_extra_nodes, cfg.replicate_growth_pct, initial_nodes);
        printf("cpr: initial cutsize=%d  max_allowed=%d (+%d%%)\n",
               initial_cutsize, max_allowed_cutsize, cfg.cutsize_growth_pct);
        printf("cpr: relocate rounds<=%d stall=%d; replicate rounds<=%d stall=%d\n",
               cfg.relocate_max_rounds, cfg.relocate_stall_limit,
               cfg.replicate_max_rounds, cfg.replicate_stall_limit);
    }

    SimpleTimer timer(pNtk);

    run_relocate_phase(pNtk, timer, num_parts, cfg, max_allowed_cutsize);
    run_replicate_phase(pNtk, timer, cfg, max_allowed_cutsize, extra_nodes, max_extra_nodes);

    // Restore the partition-balance invariant. Intermediate rounds are allowed
    // to violate it; the final network must satisfy balance_pct. Compute a
    // timing snapshot first so the repair can prefer non-critical candidates
    // when picking nodes to move.
    timer.compute_arrival();
    enforce_balance(pNtk, num_parts, balance_pct, timer.get_arrival(), cfg.verbose);
    Abc_NtkUpdateCutNets(pNtk);

    // Final timing report.
    timer.compute_arrival();
    float final_max = timer.max_arrival();
    int   final_cut = Abc_NtkComputeCutSize(pNtk);
    if (cfg.verbose)
    {
        printf("cpr: final max arrival = %.2f  (added %d node(s), +%.2f%%)\n",
               final_max, extra_nodes,
               initial_nodes > 0 ? 100.0 * extra_nodes / initial_nodes : 0.0);
        printf("cpr: final cutsize = %d (initial %d, cap %d)\n",
               final_cut, initial_cutsize, max_allowed_cutsize);
    }

    // Persist the (possibly updated) balance target back into the pdb so that
    // subsequent commands see the actual constraint used.
    pNtk->pPdb->set_balance_pct(balance_pct);

    return true;
}

} // namespace fox::cpr
