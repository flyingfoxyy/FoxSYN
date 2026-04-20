#include "cpr.hpp"

#include "timer/timer.hpp"

#include "base/abc/abc.h"
#include "base/abc/abcPdb.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <utility>
#include <vector>

namespace fox::cpr {

using ::fox::timer::SimpleTimer;

static constexpr float EPS = 1e-3f;
static constexpr int   DEFAULT_BALANCE_PCT = 2;

enum class SegmentMoveKind {
    ToSrc,
    ToSink,
    SplitMid,
};

// Per-replicate bookkeeping so a round can be rolled back if it breaches the
// cut-net cap. Each entry is one accepted duplication within try_replicate.
struct ReplicateEntry {
    Abc_Obj_t *pObj;
    Abc_Obj_t *pDup;
    std::vector<Abc_Obj_t *> patched_fanouts;
};

struct RelocateSegment {
    int index = -1;
    part_id part = ABC_PART_ID_NONE;
    part_id src_part = ABC_PART_ID_NONE;
    part_id sink_part = ABC_PART_ID_NONE;
    std::vector<Abc_Obj_t *> nodes;
};

struct SegmentMove {
    int segment_idx = -1;
    SegmentMoveKind kind = SegmentMoveKind::ToSrc;
    std::vector<Abc_Obj_t *> nodes;
    std::vector<part_id> old_parts;
    std::vector<part_id> new_parts;
    int hop_gain = 0;
    int new_hops = 0;
    float score = -1.0f;
    int cut_delta = 0;
    int balance_penalty = 0;
    int cost = 0;
    int new_cut = 0;
};

static bool is_part_stat_vertex(Abc_Obj_t *pObj)
{
    return pObj
        && (Abc_ObjIsPi(pObj)
         || Abc_ObjIsNode(pObj)
         || Abc_ObjType(pObj) == ABC_OBJ_CONST1);
}

struct LocalCutNetState {
    std::vector<char> is_cut_net;
    int cut_size = 0;

    void recompute(Abc_Ntk_t *pNtk)
    {
        cut_size = 0;
        if (!pNtk)
        {
            is_cut_net.clear();
            return;
        }

        is_cut_net.assign(Abc_NtkObjNumMax(pNtk), 0);
        int i, k;
        Abc_Obj_t *pObj;
        Abc_Obj_t *pFanin;
        Abc_NtkForEachObj(pNtk, pObj, i)
        {
            if (!is_part_stat_vertex(pObj))
                continue;
            part_id obj_part = Abc_ObjGetPartId(pObj);
            if (obj_part == ABC_PART_ID_NONE)
                continue;

            Abc_ObjForEachFanin(pObj, pFanin, k)
            {
                if (!is_part_stat_vertex(pFanin))
                    continue;
                part_id fanin_part = Abc_ObjGetPartId(pFanin);
                if (fanin_part == ABC_PART_ID_NONE || fanin_part == obj_part)
                    continue;
                if (!is_cut_net[pFanin->Id])
                {
                    is_cut_net[pFanin->Id] = 1;
                    cut_size += 1;
                }
            }
        }
    }

    bool is_cut(Abc_Obj_t *pObj) const
    {
        return pObj && pObj->Id >= 0
            && static_cast<size_t>(pObj->Id) < is_cut_net.size()
            && is_cut_net[pObj->Id] != 0;
    }
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

static const char *segment_move_name(SegmentMoveKind kind)
{
    switch (kind)
    {
    case SegmentMoveKind::ToSrc:
        return "to-src";
    case SegmentMoveKind::ToSink:
        return "to-sink";
    case SegmentMoveKind::SplitMid:
        return "split";
    }
    return "?";
}

static void ordered_path_nodes(Abc_Ntk_t *pNtk, const fox::timer::Path &path,
                               std::vector<Abc_Obj_t *> &nodes)
{
    nodes.clear();
    for (int id : path.ids)
    {
        Abc_Obj_t *pObj = Abc_NtkObj(pNtk, id);
        if (pObj && Abc_ObjIsNode(pObj))
            nodes.push_back(pObj);
    }
}

static int count_path_hops(Abc_Ntk_t *pNtk, const std::vector<int> &path_ids)
{
    int hops = 0;
    for (size_t i = 1; i < path_ids.size(); ++i)
    {
        Abc_Obj_t *pFrom = Abc_NtkObj(pNtk, path_ids[i - 1]);
        Abc_Obj_t *pTo = Abc_NtkObj(pNtk, path_ids[i]);
        if (!pFrom || !pTo)
            continue;

        part_id from_part = Abc_ObjGetPartId(pFrom);
        part_id to_part = Abc_ObjGetPartId(pTo);
        if (from_part != ABC_PART_ID_NONE && to_part != ABC_PART_ID_NONE
            && from_part != to_part)
        {
            hops += 1;
        }
    }
    return hops;
}

static void extract_segments(const std::vector<Abc_Obj_t *> &path_nodes,
                             std::vector<RelocateSegment> &segments)
{
    segments.clear();
    if (path_nodes.empty())
        return;

    RelocateSegment cur;
    cur.part = Abc_ObjGetPartId(path_nodes.front());
    cur.nodes.push_back(path_nodes.front());

    for (size_t i = 1; i < path_nodes.size(); ++i)
    {
        Abc_Obj_t *pObj = path_nodes[i];
        part_id part = Abc_ObjGetPartId(pObj);
        if (part == cur.part)
        {
            cur.nodes.push_back(pObj);
            continue;
        }

        cur.index = static_cast<int>(segments.size());
        segments.push_back(std::move(cur));
        cur = RelocateSegment{};
        cur.part = part;
        cur.nodes.push_back(pObj);
    }
    cur.index = static_cast<int>(segments.size());
    segments.push_back(std::move(cur));

    for (size_t i = 0; i < segments.size(); ++i)
    {
        segments[i].index = static_cast<int>(i);
        if (i > 0)
            segments[i].src_part = segments[i - 1].part;
        if (i + 1 < segments.size())
            segments[i].sink_part = segments[i + 1].part;
    }
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

// Count objects in each partition, using the same accounting as hpart
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

static int compute_balance_max_allowed(const std::vector<int> &sz, int balance_pct)
{
    if (sz.empty())
        return 0;

    int total = 0;
    for (int s : sz) total += s;
    if (total <= 0)
        return 0;

    int avg = total / static_cast<int>(sz.size());
    int slack = (avg * balance_pct + 99) / 100;
    return std::max(avg + slack, avg + 1);
}

static int compute_balance_overflow(const std::vector<int> &sz, int max_allowed)
{
    if (max_allowed <= 0)
        return 0;

    int overflow = 0;
    for (int s : sz)
        overflow += std::max(0, s - max_allowed);
    return overflow;
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

static bool build_whole_segment_move(const RelocateSegment &seg, part_id target,
                                     SegmentMoveKind kind, SegmentMove &move)
{
    if (seg.nodes.empty() || target == ABC_PART_ID_NONE || target == seg.part)
        return false;

    move = SegmentMove{};
    move.segment_idx = seg.index;
    move.kind = kind;
    move.nodes = seg.nodes;
    move.old_parts.reserve(seg.nodes.size());
    move.new_parts.reserve(seg.nodes.size());
    for (Abc_Obj_t *pObj : seg.nodes)
    {
        move.old_parts.push_back(Abc_ObjGetPartId(pObj));
        move.new_parts.push_back(target);
    }
    return true;
}

static bool build_split_segment_move(const RelocateSegment &seg, SegmentMove &move)
{
    if (seg.nodes.size() < 2 || seg.src_part == ABC_PART_ID_NONE
        || seg.sink_part == ABC_PART_ID_NONE || seg.src_part == seg.sink_part)
    {
        return false;
    }

    const int split = static_cast<int>(seg.nodes.size() / 2);
    if (split <= 0 || split >= static_cast<int>(seg.nodes.size()))
        return false;

    move = SegmentMove{};
    move.segment_idx = seg.index;
    move.kind = SegmentMoveKind::SplitMid;
    move.nodes = seg.nodes;
    move.old_parts.reserve(seg.nodes.size());
    move.new_parts.reserve(seg.nodes.size());
    for (size_t i = 0; i < seg.nodes.size(); ++i)
    {
        move.old_parts.push_back(Abc_ObjGetPartId(seg.nodes[i]));
        move.new_parts.push_back(static_cast<int>(i) < split ? seg.src_part : seg.sink_part);
    }
    return true;
}

static void apply_segment_move(const SegmentMove &move)
{
    for (size_t i = 0; i < move.nodes.size(); ++i)
        Abc_ObjSetPartId(move.nodes[i], move.new_parts[i]);
}

static void revert_segment_move(const SegmentMove &move)
{
    for (size_t i = 0; i < move.nodes.size(); ++i)
        Abc_ObjSetPartId(move.nodes[i], move.old_parts[i]);
}

static bool is_better_segment_move(const SegmentMove &cand, const SegmentMove &best)
{
    if (cand.score > best.score + EPS)
        return true;
    if (cand.score + EPS < best.score)
        return false;

    if (cand.hop_gain > best.hop_gain)
        return true;
    if (cand.hop_gain < best.hop_gain)
        return false;

    if (cand.cut_delta < best.cut_delta)
        return true;
    if (cand.cut_delta > best.cut_delta)
        return false;

    if (cand.nodes.size() < best.nodes.size())
        return true;
    if (cand.nodes.size() > best.nodes.size())
        return false;

    return cand.segment_idx < best.segment_idx;
}

static bool evaluate_segment_move(Abc_Ntk_t *pNtk, const SegmentMove &shape,
                                  const std::vector<int> &path_ids, int base_hops, int base_cut,
                                  const std::vector<int> &base_sizes,
                                  int balance_max_allowed, int num_parts,
                                  int max_allowed_cutsize, SegmentMove &evaluated)
{
    evaluated = shape;
    apply_segment_move(shape);

    LocalCutNetState cut_nets;
    cut_nets.recompute(pNtk);
    const int cur_cut = cut_nets.cut_size;
    bool ok = false;
    if (cur_cut <= max_allowed_cutsize)
    {
        const int new_hops = count_path_hops(pNtk, path_ids);
        const int hop_gain = base_hops - new_hops;
        if (hop_gain > 0)
        {
            std::vector<int> sz = base_sizes;
            for (size_t i = 0; i < shape.old_parts.size(); ++i)
            {
                part_id old_part = shape.old_parts[i];
                part_id new_part = shape.new_parts[i];
                if (old_part != ABC_PART_ID_NONE && static_cast<int>(old_part) < num_parts)
                    sz[old_part] -= 1;
                if (new_part != ABC_PART_ID_NONE && static_cast<int>(new_part) < num_parts)
                    sz[new_part] += 1;
            }

            evaluated.hop_gain = hop_gain;
            evaluated.new_hops = new_hops;
            evaluated.new_cut = cur_cut;
            evaluated.cut_delta = cur_cut - base_cut;
            evaluated.balance_penalty = compute_balance_overflow(sz, balance_max_allowed);
            evaluated.cost = 1 + std::max(0, evaluated.cut_delta) + evaluated.balance_penalty;
            evaluated.score = static_cast<float>(evaluated.hop_gain)
                / static_cast<float>(evaluated.cost);
            ok = true;
        }
    }

    revert_segment_move(shape);
    return ok;
}

static bool find_best_segment_move(Abc_Ntk_t *pNtk,
                                   const std::vector<RelocateSegment> &segments,
                                   const std::vector<int> &path_ids,
                                   int base_hops, int base_cut,
                                   const std::vector<int> &base_sizes,
                                   int balance_pct, int num_parts,
                                   int max_allowed_cutsize, SegmentMove &best)
{
    const int balance_max_allowed = compute_balance_max_allowed(base_sizes, balance_pct);
    bool found = false;

    for (const RelocateSegment &seg : segments)
    {
        if (seg.index == 0 || seg.index + 1 >= static_cast<int>(segments.size()))
            continue;

        SegmentMove shape;
        SegmentMove evaluated;

        if (build_whole_segment_move(seg, seg.src_part, SegmentMoveKind::ToSrc, shape)
            && evaluate_segment_move(pNtk, shape, path_ids, base_hops, base_cut, base_sizes,
                                     balance_max_allowed, num_parts, max_allowed_cutsize, evaluated)
            && (!found || is_better_segment_move(evaluated, best)))
        {
            best = std::move(evaluated);
            found = true;
        }

        if (seg.sink_part != seg.src_part
            && build_whole_segment_move(seg, seg.sink_part, SegmentMoveKind::ToSink, shape)
            && evaluate_segment_move(pNtk, shape, path_ids, base_hops, base_cut, base_sizes,
                                     balance_max_allowed, num_parts, max_allowed_cutsize, evaluated)
            && (!found || is_better_segment_move(evaluated, best)))
        {
            best = std::move(evaluated);
            found = true;
        }

        if (build_split_segment_move(seg, shape)
            && evaluate_segment_move(pNtk, shape, path_ids, base_hops, base_cut, base_sizes,
                                     balance_max_allowed, num_parts, max_allowed_cutsize, evaluated)
            && (!found || is_better_segment_move(evaluated, best)))
        {
            best = std::move(evaluated);
            found = true;
        }
    }
    return found;
}

static void revert_replicate_round(std::vector<ReplicateEntry> &entries)
{
    for (auto it = entries.rbegin(); it != entries.rend(); ++it)
    {
        for (Abc_Obj_t *pF : it->patched_fanouts)
            Abc_ObjPatchFanin(pF, it->pDup, it->pObj);
        Abc_NtkDeleteObj(it->pDup);
    }
    entries.clear();
}

static bool try_replicate_on_path(Abc_Obj_t *pObj, Abc_Obj_t *pSucc, std::vector<int> &path_ids,
                                  int path_idx, Abc_Ntk_t *pNtk, int budget,
                                  LocalCutNetState &cut_nets, int &path_hops,
                                  std::vector<ReplicateEntry> *pLedger)
{
    if (budget <= 0 || !pObj || !pSucc)
        return false;
    if (!cut_nets.is_cut(pObj))
        return false;

    part_id my_part = Abc_ObjGetPartId(pObj);
    part_id target_part = Abc_ObjGetPartId(pSucc);
    if (my_part == ABC_PART_ID_NONE || target_part == ABC_PART_ID_NONE || my_part == target_part)
        return false;
    if (path_idx < 0 || path_idx + 1 >= static_cast<int>(path_ids.size()))
        return false;
    if (path_ids[path_idx] != pObj->Id || path_ids[path_idx + 1] != pSucc->Id)
        return false;

    std::vector<Abc_Obj_t *> fanouts;
    Abc_Obj_t *pFanout;
    int i;
    Abc_ObjForEachFanout(pObj, pFanout, i)
    {
        if (Abc_ObjGetPartId(pFanout) == target_part)
            fanouts.push_back(pFanout);
    }
    if (fanouts.empty())
        return false;

    Abc_Obj_t *pDup = duplicate_node(pNtk, pObj);
    if (!pDup)
        return false;
    Abc_ObjSetPartId(pDup, target_part);

    for (Abc_Obj_t *pF : fanouts)
        Abc_ObjPatchFanin(pF, pObj, pDup);

    std::vector<int> new_path_ids = path_ids;
    new_path_ids[path_idx] = pDup->Id;
    int new_hops = count_path_hops(pNtk, new_path_ids);
    if (new_hops < path_hops)
    {
        path_hops = new_hops;
        path_ids.swap(new_path_ids);
        cut_nets.recompute(pNtk);
        if (pLedger)
            pLedger->push_back({pObj, pDup, fanouts});
        return true;
    }

    for (Abc_Obj_t *pF : fanouts)
        Abc_ObjPatchFanin(pF, pDup, pObj);
    Abc_NtkDeleteObj(pDup);
    cut_nets.recompute(pNtk);
    return false;
}

static void run_relocate_phase(Abc_Ntk_t *pNtk, SimpleTimer &timer, int num_parts,
                               const Config &cfg, int balance_pct, int max_allowed_cutsize)
{
    int stall = 0;
    for (int round = 0; round < cfg.relocate_max_rounds; ++round)
    {
        timer.compute_arrival();

        auto paths = timer.extract_top_paths(1);
        if (paths.empty())
            break;
        const std::vector<int> &path_ids = paths.front().ids;
        const int base_hops = count_path_hops(pNtk, path_ids);

        std::vector<Abc_Obj_t *> path_nodes;
        ordered_path_nodes(pNtk, paths.front(), path_nodes);
        if (path_nodes.empty())
            break;

        std::vector<RelocateSegment> segments;
        extract_segments(path_nodes, segments);
        if (segments.size() < 2)
            break;

        LocalCutNetState base_cut_nets;
        base_cut_nets.recompute(pNtk);
        int base_cut = base_cut_nets.cut_size;
        std::vector<int> base_sizes;
        partition_sizes(pNtk, num_parts, base_sizes);

        SegmentMove best;
        if (!find_best_segment_move(pNtk, segments, path_ids, base_hops, base_cut, base_sizes,
                                    balance_pct, num_parts, max_allowed_cutsize, best))
        {
            if (++stall >= cfg.relocate_stall_limit)
            {
                if (cfg.verbose)
                    printf("cpr: relocate stalled for %d rounds, stopping phase\n", stall);
                break;
            }
            continue;
        }

        apply_segment_move(best);
        timer.compute_arrival();
        LocalCutNetState cur_cut_nets;
        cur_cut_nets.recompute(pNtk);
        int cur_cut = cur_cut_nets.cut_size;
        if (cur_cut > max_allowed_cutsize)
        {
            revert_segment_move(best);
            timer.compute_arrival();
            if (cfg.verbose)
                printf("cpr: relocate round %d breached cut cap (%d>%d); reverted, stopping phase\n",
                       round, cur_cut, max_allowed_cutsize);
            break;
        }

        timer.compute_arrival();
        if (cfg.verbose)
        {
            const int net_hops = Abc_NtkComputeHopNum(pNtk);
            printf("cpr: relocate round %3d  segment=%3d  move=%-7s  nodes=%3zu"
                   "  score=%7.4f  hop=%3d->%-3d  net=%3d  cost=%3d  cut=%4d/%-4d\n",
                   round, best.segment_idx, segment_move_name(best.kind), best.nodes.size(),
                   best.score, base_hops, best.new_hops, net_hops, best.cost,
                   cur_cut, max_allowed_cutsize);
        }

        stall = 0;
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
        auto paths = timer.extract_top_paths(1);
        if (paths.empty())
            break;
        std::vector<int> path_ids = paths.front().ids;
        int prev_hops = count_path_hops(pNtk, path_ids);
        if (prev_hops <= 0)
            break;
        LocalCutNetState cut_nets;
        cut_nets.recompute(pNtk);

        std::vector<ReplicateEntry> entries;
        int round_adds = 0;
        int cur_hops = prev_hops;
        for (int idx = 1; idx + 1 < static_cast<int>(path_ids.size()); ++idx)
        {
            remaining = max_extra_nodes - extra_nodes;
            if (remaining <= 0)
                break;

            Abc_Obj_t *pObj = Abc_NtkObj(pNtk, path_ids[idx]);
            Abc_Obj_t *pSucc = Abc_NtkObj(pNtk, path_ids[idx + 1]);
            if (!pObj || !Abc_ObjIsNode(pObj))
                continue;

            if (try_replicate_on_path(pObj, pSucc, path_ids, idx, pNtk, remaining, cut_nets,
                                      cur_hops, &entries))
            {
                extra_nodes += 1;
                round_adds += 1;
            }
        }

        int cur_cut = cut_nets.cut_size;
        if (cur_cut > max_allowed_cutsize)
        {
            revert_replicate_round(entries);
            extra_nodes -= round_adds;
            cut_nets.recompute(pNtk);
            timer.compute_arrival();
            if (cfg.verbose)
                printf("cpr: replicate round %d breached cut cap (%d>%d); reverted, stopping phase\n",
                       round, cur_cut, max_allowed_cutsize);
            break;
        }

        timer.compute_arrival();
        if (cfg.verbose)
        {
            const int net_hops = Abc_NtkComputeHopNum(pNtk);
            printf("cpr: replicate round %3d  adds=%3d  hop=%3d->%-3d"
                   "  net=%3d  cut=%4d/%-4d  nodes=%3d/%-3d\n",
                   round, round_adds, prev_hops, cur_hops, net_hops,
                   cur_cut, max_allowed_cutsize, extra_nodes, max_extra_nodes);
        }

        if (cur_hops < prev_hops)
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
    LocalCutNetState initial_cut_nets;
    initial_cut_nets.recompute(pNtk);
    const int initial_cutsize = initial_cut_nets.cut_size;
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

    run_relocate_phase(pNtk, timer, num_parts, cfg, balance_pct, max_allowed_cutsize);
    run_replicate_phase(pNtk, timer, cfg, max_allowed_cutsize, extra_nodes, max_extra_nodes);

    // Restore the partition-balance invariant. Intermediate rounds are allowed
    // to violate it; the final network must satisfy balance_pct. Compute a
    // timing snapshot first so the repair can prefer non-critical candidates
    // when picking nodes to move.
    timer.compute_arrival();
    enforce_balance(pNtk, num_parts, balance_pct, timer.get_arrival(), cfg.verbose);

    // Final timing report.
    timer.compute_arrival();
    float final_max = timer.max_arrival();
    LocalCutNetState final_cut_nets;
    final_cut_nets.recompute(pNtk);
    int   final_cut = final_cut_nets.cut_size;
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
