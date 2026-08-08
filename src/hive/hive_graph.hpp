#ifndef HIVE_GRAPH_HPP
#define HIVE_GRAPH_HPP

#include <vector>

#include "base/abc/abc.h"

namespace fox::hive {
// Combinational-graph snapshot of a logic network (docs/hive-design.md 5.1).
// Vertices: every internal node (Abc_ObjIsNode), constants included.
// PI/PO/BI/BO/latch are boundary objects, never vertices; traversal stops at
// them, which is what makes a latch break combinational paths. Adjacency is
// deduplicated by object id. rank() strictly increases along every edge and
// is independent of ABC's Level field. Snapshot: built once, never updated.
class CombGraph {
public:
    explicit CombGraph(Abc_Ntk_t *pNtk);

    Abc_Ntk_t *ntk() const { return m_ntk; }
    int max_id() const { return m_max_id; }
    bool acyclic() const { return m_acyclic; }
    bool is_vertex(int id) const { return m_is_vertex[id] != 0; }
    int rank(int id) const { return m_rank[id]; }
    const std::vector<int> &vertices() const { return m_vertices; }
    const std::vector<int> &preds(int id) const { return m_preds[id]; }
    const std::vector<int> &succs(int id) const { return m_succs[id]; }
    const std::vector<int> &ext_ins(int id) const { return m_ext_ins[id]; }
    bool has_ext_out(int id) const { return m_ext_out[id] != 0; }

private:
    Abc_Ntk_t *m_ntk = nullptr;
    int m_max_id = 0;
    bool m_acyclic = true;
    std::vector<char> m_is_vertex, m_ext_out;
    std::vector<int> m_rank;
    std::vector<int> m_vertices;
    std::vector<std::vector<int>> m_preds, m_succs, m_ext_ins;
};

} // namespace fox::hive

#endif // HIVE_GRAPH_HPP
