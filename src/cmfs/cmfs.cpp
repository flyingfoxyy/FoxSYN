#include "cmfs.hpp"

#include "timer/timer.hpp"
#include "base/abc/abc.h"
#include "base/abc/abcPdb.hpp"

#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <vector>

extern "C" {
#include "opt/mfs/mfsInt.h"

int Abc_WinNode(Mfs_Man_t *p, Abc_Obj_t *pNode);
int Abc_NtkMfsSolveSatResub(Mfs_Man_t *p, Abc_Obj_t *pNode,
                             int iFanin, int fOnlyRemove, int fSkipUpdate);
}

namespace fox::cmfs {

using ::fox::timer::SimpleTimer;

static constexpr float HOP_DLY = 200.0f;

struct CandidateEdge {
    int node_id;
    int iFanin;
    float slack;
    int frequency;
    float weight;
};

static float edge_delay(Abc_Obj_t *pFrom, Abc_Obj_t *pTo, Pdb *pPdb)
{
    if (!pPdb)
        return 0.0f;
    part_id fp = pPdb->get(pFrom->Id);
    part_id tp = pPdb->get(pTo->Id);
    if (fp == ABC_PART_ID_NONE || tp == ABC_PART_ID_NONE)
        return 0.0f;
    return (fp != tp) ? HOP_DLY : 0.0f;
}

static void collect_candidates(Abc_Ntk_t *pNtk, const std::vector<float> &arrival,
                               const std::vector<fox::timer::Path> &paths,
                               std::vector<CandidateEdge> &candidates)
{
    Pdb *pPdb = pNtk->pPdb;
    std::unordered_map<uint64_t, CandidateEdge> edge_map;

    for (const auto &path : paths)
    {
        for (int id : path.ids)
        {
            Abc_Obj_t *pNode = Abc_NtkObj(pNtk, id);
            if (!pNode || !Abc_ObjIsNode(pNode))
                continue;
            if (Abc_ObjFaninNum(pNode) < 2)
                continue;

            float best = -1.0f, second = -1.0f;
            int best_idx = -1;
            Abc_Obj_t *pFi;
            int k;
            Abc_ObjForEachFanin(pNode, pFi, k)
            {
                float c = arrival[pFi->Id] + edge_delay(pFi, pNode, pPdb);
                if (c > best)
                {
                    second = best;
                    best = c;
                    best_idx = k;
                }
                else if (c > second)
                {
                    second = c;
                }
            }

            float slack = best - second;
            if (slack < 0.5f || best_idx < 0)
                continue;

            uint64_t key = (static_cast<uint64_t>(pNode->Id) << 16)
                         | static_cast<uint64_t>(best_idx);
            auto &entry = edge_map[key];
            entry.node_id = pNode->Id;
            entry.iFanin = best_idx;
            entry.slack = slack;
            entry.frequency += 1;
        }
    }

    candidates.clear();
    candidates.reserve(edge_map.size());
    for (auto &[key, e] : edge_map)
    {
        e.weight = e.slack * static_cast<float>(e.frequency);
        candidates.push_back(e);
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const CandidateEdge &a, const CandidateEdge &b) {
                  return a.weight > b.weight;
              });
}

bool ApplyCmfs(Abc_Ntk_t *pNtk, const Config &cfg)
{
    if (!pNtk)
    {
        printf("cmfs: network is null\n");
        return false;
    }
    if (!Abc_NtkIsLogic(pNtk))
    {
        printf("cmfs: network must be logic (not AIG)\n");
        return false;
    }
    if (!pNtk->pPdb)
    {
        printf("cmfs: no partition database (run hpart first)\n");
        return false;
    }

    // Force a clean Hop manager: convert to SOP first, then back to AIG.
    Abc_NtkToSop(pNtk, -1, ABC_INFINITY);
    if (!Abc_NtkToAig(pNtk))
    {
        printf("cmfs: failed to convert to AIG representation\n");
        return false;
    }

    int total_attempts = 0, total_successes = 0, total_timeouts = 0;

    SimpleTimer timer(pNtk);
    timer.compute_arrival();
    float initial_arrival = timer.max_arrival();
    float best_arrival = initial_arrival;
    int stall_count = 0;

    if (cfg.verbose)
    {
        printf("cmfs: initial max arrival = %.2f\n", initial_arrival);
        printf("cmfs: top_K=%d  max_rounds=%d  stall=%d  BT=%d\n",
               cfg.top_K, cfg.max_rounds, cfg.stall_limit, cfg.nBTLimit);
        fflush(stdout);
    }

    for (int round = 0; round < cfg.max_rounds; ++round)
    {
        // Re-establish clean Hop manager each round (sweep/cleanup may invalidate it)
        Abc_NtkToSop(pNtk, -1, ABC_INFINITY);
        if (!Abc_NtkToAig(pNtk))
            break;

        Abc_NtkLevel(pNtk);
        Abc_NtkStartReverseLevels(pNtk, 0);

        SimpleTimer rtimer(pNtk);
        rtimer.compute_arrival();
        auto paths = rtimer.extract_top_paths(cfg.top_K);
        if (paths.empty())
        {
            Abc_NtkStopReverseLevels(pNtk);
            break;
        }

        std::vector<CandidateEdge> candidates;
        collect_candidates(pNtk, rtimer.get_arrival(), paths, candidates);

        if (candidates.empty())
        {
            Abc_NtkStopReverseLevels(pNtk);
            break;
        }

        Mfs_Par_t pars;
        Abc_NtkMfsParsDefault(&pars);
        pars.fResub      = 1;
        pars.nBTLimit    = cfg.nBTLimit;
        pars.nWinTfoLevs = cfg.nWinTfoLevs;
        pars.nFanoutsMax = cfg.nFanoutsMax;
        pars.nWinMax     = cfg.nWinMax;

        Mfs_Man_t *p = Mfs_ManAlloc(&pars);
        p->pNtk = pNtk;
        p->nFaninMax = Abc_NtkGetFaninMax(pNtk);

        int round_successes = 0;
        std::vector<char> node_done(Abc_NtkObjNumMax(pNtk), 0);

        for (const auto &cand : candidates)
        {
            if (static_cast<size_t>(cand.node_id) < node_done.size()
                && node_done[cand.node_id])
                continue;

            Abc_Obj_t *pNode = Abc_NtkObj(pNtk, cand.node_id);
            if (!pNode || !Abc_ObjIsNode(pNode))
                continue;
            if (Abc_ObjFaninNum(pNode) < 2)
                continue;
            if (cand.iFanin >= Abc_ObjFaninNum(pNode))
                continue;
            if (Abc_ObjFaninNum(pNode) > p->nFaninMax)
                continue;

            if (Abc_WinNode(p, pNode) != 0)
                continue;

            part_id orig_part = Abc_ObjGetPartId(pNode);
            int id_before = Abc_NtkObjNumMax(pNtk);

            total_attempts++;
            int ret = Abc_NtkMfsSolveSatResub(p, pNode, cand.iFanin, 1, 0);
            if (ret == 1)
            {
                total_successes++;
                round_successes++;
                Abc_Obj_t *pNew = Abc_NtkObj(pNtk, id_before);
                if (pNew && orig_part != ABC_PART_ID_NONE)
                    Abc_ObjSetPartId(pNew, orig_part);
                if (static_cast<size_t>(cand.node_id) < node_done.size())
                    node_done[cand.node_id] = 1;
            }
        }

        total_timeouts += p->nTimeOuts;
        Mfs_ManStop(p);
        Abc_NtkStopReverseLevels(pNtk);

        Abc_NtkSweep(pNtk, 0);
        Abc_NtkCleanup(pNtk, 0);

        SimpleTimer ptimer(pNtk);
        ptimer.compute_arrival();
        float new_arrival = ptimer.max_arrival();

        if (cfg.verbose)
        {
            printf("cmfs: round %2d  candidates=%3zu  removed=%3d  arrival=%.2f\n",
                   round, candidates.size(), round_successes, new_arrival);
        }

        if (new_arrival < best_arrival - 0.5f)
        {
            best_arrival = new_arrival;
            stall_count = 0;
        }
        else
        {
            stall_count++;
            if (stall_count >= cfg.stall_limit)
                break;
        }

        if (round_successes == 0)
            break;
    }

    // Final report
    SimpleTimer ftimer(pNtk);
    ftimer.compute_arrival();
    float final_arrival = ftimer.max_arrival();

    printf("cmfs: %d rounds, %d attempts, %d removals (%d timeouts)\n",
           stall_count > 0 ? stall_count : cfg.max_rounds,
           total_attempts, total_successes, total_timeouts);
    printf("cmfs: arrival %.2f -> %.2f (improvement %.2f)\n",
           initial_arrival, final_arrival, initial_arrival - final_arrival);

    return true;
}

} // namespace fox::cmfs


