#ifndef CSR3_INTERNAL_HPP
#define CSR3_INTERNAL_HPP

#include <vector>
#include "base/abc/abc.h"

namespace fox::csr3 {

// A crossing line plus its partition-bounded support (sorted leaf ObjIds).
struct Line {
    Abc_Obj_t         *driver = nullptr;
    std::vector<int>   support;   // sorted, unique leaf ObjIds
};

struct Group {
    std::vector<Abc_Obj_t*> lines;   // <= max_lines drivers, k = lines.size()
};

// Per-group measurement result.
struct GroupResult {
    int  k           = 0;      // number of lines in the group
    long m           = 0;      // reachable output combinations (>=1); may be capped (see prefiltered)
    int  gain        = 0;      // k - ceil_log2(m), >= 0
    int  support_size= 0;      // |union of supports|
    bool prefiltered = false;  // true = simulation ruled it out (m treated as 2^k, gain=0)
};

// Task 2
std::vector<Abc_Obj_t*> collect_crossing_signals(Abc_Ntk_t *pNtk, int srcPart);
// Task 3
std::vector<int>        extract_support_partition_aware(Abc_Obj_t *line, int srcPart);
bool                    is_cone_leaf(Abc_Obj_t *pObj, int srcPart);
// Task 4
std::vector<Group>      group_by_jaccard(const std::vector<Line> &lines, int jaccardPct, int kmax);
// Task 5
Abc_Ntk_t *             build_group_cone_ntk(const std::vector<Abc_Obj_t*> &lines, int srcPart);
// Task 6
long                    simulate_prefilter(Abc_Ntk_t *pCone, int k, int nWords);
// Task 7
long                    count_m_sat(Abc_Ntk_t *pCone, int k, int btlimit);      // exact m; > (long)1<<(k-1) => early-exit sentinel; -1 => timeout
long                    count_m_exhaustive(Abc_Ntk_t *pCone, int k);            // exact m via full 2^support sim (support<=16)

// helpers
int  ceil_log2(long m);   // ceil(log2(m)); ceil_log2(1)=0

} // namespace fox::csr3
#endif // CSR3_INTERNAL_HPP
