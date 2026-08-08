#ifndef FMPART_ABC_WRAPPER_HPP
#define FMPART_ABC_WRAPPER_HPP

#include <vector>

#include "base/abc/abc.h"

namespace fox::fmpart {

// Hypergraph view of Abc_Ntk_t; construction matches hpart's BuildHypergraph
// (hpart.cpp:164; known duplication, see docs/fmpart-design.md §7):
// vertices = PI / node / latch / const1; one hyperedge per driver covering its
// transitively reachable sinks; edges with fewer than 2 pins are dropped.
// Snapshot semantics: built once at construction; later network edits are ignored.
class AbcNtkWrapper {
public:
    explicit AbcNtkWrapper(Abc_Ntk_t *pNtk);

    int num_vertices() const { return (int)m_vertices.size(); }
    int num_nets() const { return (int)m_pins.size(); }
    int vertex_weight(int) const { return 1; }
    int net_weight(int) const { return 1; }
    const std::vector<int> &pins_of(int e) const { return m_pins[e]; }

    // Callers write results back (e.g. to Pdb) via this map; this module does not
    Abc_Obj_t *vertex_to_obj(int v) const { return m_vertices[v]; }

private:
    std::vector<Abc_Obj_t *> m_vertices;
    std::vector<std::vector<int>> m_pins;
};

} // namespace fox::fmpart

#endif // FMPART_ABC_WRAPPER_HPP
