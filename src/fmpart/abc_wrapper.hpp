#ifndef FMPART_ABC_WRAPPER_HPP
#define FMPART_ABC_WRAPPER_HPP

#include <vector>

#include "base/abc/abc.h"

namespace fox::fmpart {

// Abc_Ntk_t 的超图视图，建图口径与 hpart 的 BuildHypergraph 一致
// （hpart.cpp:164，已知重复见 docs/fmpart-design.md §7）:
// 顶点 = PI / node / latch / const1；每个 driver 一条超边，
// 覆盖其传递可达的 sink；不足 2 pin 的边丢弃。
// 快照语义：构造时建一次，之后网络的修改不反映进来。
class AbcNtkWrapper {
public:
    explicit AbcNtkWrapper(Abc_Ntk_t *pNtk);

    int num_vertices() const { return (int)m_vertices.size(); }
    int num_nets() const { return (int)m_pins.size(); }
    int vertex_weight(int) const { return 1; }
    int net_weight(int) const { return 1; }
    const std::vector<int> &pins_of(int e) const { return m_pins[e]; }

    // 结果回写（如写 Pdb）由调用方经此映射完成，本模块不写回
    Abc_Obj_t *vertex_to_obj(int v) const { return m_vertices[v]; }

private:
    std::vector<Abc_Obj_t *> m_vertices;
    std::vector<std::vector<int>> m_pins;
};

} // namespace fox::fmpart

#endif // FMPART_ABC_WRAPPER_HPP
