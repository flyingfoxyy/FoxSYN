#ifndef CSR4_INTERNAL_HPP
#define CSR4_INTERNAL_HPP

#include <cstdint>
#include <vector>

#include "csr4/csr4.hpp"
#include "base/abc/abc.h"

namespace fox::csr4 {

// Truth table must fit one uint64_t, so at most 6 fanins.
constexpr int CSR4_MAX_FANIN = 6;

// A destination-side LUT sitting on the partition boundary.
struct BoundaryLut {
    Abc_Obj_t        *node   = nullptr;
    std::vector<int>  fanins;        // ObjIds, in fanin order (position == truth-table var index)
    std::vector<int>  bound;         // sorted ObjIds of crossing fanins (part_id != node's part_id)
    uint64_t          truth  = 0;    // truth table over `fanins`; bit i = value at minterm i
};

// A set of consumer LUTs analyzed together under one shared encoder.
struct Group {
    std::vector<int>  luts;          // indices into the direction's BoundaryLut vector
    std::vector<int>  bound_union;   // sorted ObjIds, union of members' `bound`
};

struct GroupResult {
    int  n_luts     = 0;
    int  n_bkill    = 0;
    int  n_bkeep    = 0;
    long mu         = 0;     // joint column multiplicity over B_kill
    int  t          = 0;     // ceil_log2(mu)
    int  gain       = 0;     // n_bkill - t, clamped at 0; 0 if !k_feasible
    bool k_feasible = false;
};

// Task 1
int  ceil_log2(long m);                                     // ceil_log2(1) == 0

// Task 2
bool node_truth_u64(Abc_Obj_t *pNode, uint64_t &out);       // false if unsupported/too wide

// Task 3
std::vector<BoundaryLut> collect_boundary_luts(Abc_Ntk_t *pNtk, int dstPart, int &nSkippedWide);

// Task 4
std::vector<Group> group_boundary_luts(const std::vector<BoundaryLut> &luts,
                                       int maxBound, int maxLuts);

// Task 5
void classify_bound_nets(Abc_Ntk_t *pNtk, const std::vector<BoundaryLut> &luts,
                         const Group &g, int dstPart,
                         std::vector<int> &bkill, std::vector<int> &bkeep);

// Task 6
long joint_multiplicity(const std::vector<BoundaryLut> &luts, const Group &g,
                        const std::vector<int> &bkill);

// Task 7
bool check_k_feasible(const std::vector<BoundaryLut> &luts, const Group &g,
                      const std::vector<int> &bkill, int t, int lutSize);
GroupResult evaluate_group(Abc_Ntk_t *pNtk, const std::vector<BoundaryLut> &luts,
                           const Group &g, int dstPart, const Config &cfg);

} // namespace fox::csr4
#endif // CSR4_INTERNAL_HPP
