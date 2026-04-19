#ifndef TIMER_HPP
#define TIMER_HPP

#include "misc/util/abc_global.h"
#include "base/main/main.h"

#include <vector>

namespace fox::timer {

// A combinational path. Id order is:
//     ids[0]      -> start point (PI / CI)
//     ids[1..n-2] -> logic nodes along the path (topological order)
//     ids[n-1]    -> end point   (PO / CO)
struct Path {
    std::vector<int> ids;
    float delay = 0.0f;
};

// Simple hop-aware STA using the cpr delay model:
//   HOP_DLY = 200, LUT_DLY = 1, NET_DLY = 0.
// Only meaningful on post-techmap networks. If pNtk->pPdb is null the timer
// degenerates to pure LUT-delay counting (every net is intra-partition).
class SimpleTimer
{
public:
    explicit SimpleTimer(Abc_Ntk_t *pNtk);
    ~SimpleTimer();

    SimpleTimer(const SimpleTimer &)            = delete;
    SimpleTimer &operator=(const SimpleTimer &) = delete;

    void  compute_arrival();
    float max_arrival() const;

    // All nodes that lie on some path whose delay equals max_arrival.
    // Topological order, nodes only (no CI/CO).
    void extract_critical_path(std::vector<Abc_Obj_t *> &cpath);

    // Up to n most-critical end-to-end paths, ranked by endpoint arrival.
    std::vector<Path> extract_top_paths(int n);

    // Incrementally re-timing the transitive fanout cone of pRoot.
    void recompute_cone(Abc_Obj_t *pRoot);

    const std::vector<float> &get_arrival() const { return arrival; }
    void set_arrival(const std::vector<float> &v) { arrival = v; }

private:
    void mark_fanout_cone(Abc_Obj_t *pObj);

    Abc_Ntk_t *pNtk;
    Pdb *pPdb;
    std::vector<float> arrival;
    Vec_Ptr_t *vTopo;
};

// One-shot helper: build a timer, compute arrivals, return top-n paths.
std::vector<Path> AnalyzeCriticalPaths(Abc_Ntk_t *pNtk, int top_n);

struct Config {
    int  top_n   = 1;
    bool verbose = false;
};

bool RunTimer(Abc_Ntk_t *pNtk, const Config &cfg);

} // namespace fox::timer

#endif // TIMER_HPP
