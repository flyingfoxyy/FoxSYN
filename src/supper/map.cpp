#include <vector>
#include <ctime>

#include "base/abc/abc.h"

#include "basic.hpp"
#include "map.hpp"

using namespace abc;

namespace fox::supper {
Abc_Ntk_t *
graph_t::to_ntk()
{
    Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    if (!pNtk)
        return nullptr;

    return pNtk;
}

mapper *
mapper::create_from_aig(void *ntk)
{
    Abc_Ntk_t *pNtk = static_cast<Abc_Ntk_t *>(ntk);
    if (pNtk->ntkFunc != abc::ABC_FUNC_AIG || pNtk->ntkType != abc::ABC_NTK_STRASH) {
        std::cout << "unsupported ntk type\n";
        return nullptr;
    }

    mapper *mgr = new mapper(Abc_NtkObjNumMax(pNtk), Abc_NtkPiNum(pNtk), Abc_NtkPoNum(pNtk));

    for (int n = 0; n != pNtk->vObjs->nSize; ++n) {
        Abc_Obj_t *pObj = Abc_NtkObj(pNtk, n);
        if (!pObj) [[unlikely]] {
            mgr->add_node(graph_t::node_type_t::NONE);
            continue;
        }
        switch (pObj->Type) {
            case ABC_OBJ_CONST1:
                mgr->add_node(graph_t::node_type_t::ONE);
                break;
            case ABC_OBJ_PI:
                mgr->add_node(graph_t::node_type_t::PI);
                break;
            case ABC_OBJ_PO:
                mgr->add_node(graph_t::node_type_t::PO, Lit(Abc_ObjFaninId0(pObj), pObj->fCompl0));
                break;
            case ABC_OBJ_NODE: [[likely]]
                mgr->add_node(graph_t::node_type_t::LOGIC,
                    Lit(Abc_ObjFaninId0(pObj), pObj->fCompl0), Lit(Abc_ObjFaninId1(pObj), pObj->fCompl1));
                break;
            default:
                assert(0 && "unknown abc object type");
                mgr->add_node(graph_t::node_type_t::NONE);
                break;
        }
    }

    mgr->initialize();

    return mgr;
}

void
mapper::initialize()
{
    // move to create_from_aig ...
    for (int i = 0; i != num_nodes(); ++i) {
        const node_t &n = _nodes[i];
        for (int k = 0; k != n.size(); ++k)
            ++_int_ref[n[k].id()];
    }

    for (int i = 0; i != _est_ref.size(); ++i)
        _est_ref[i] = _int_ref[i];
}

graph_t *
mapper::run_lut_mapping(const Config &cfg)
{
    _cfg = cfg;

    // Setup PI cuts
    ForEachGraphPi(*this) {
        _cuts[_pi[idx]].push_back(allocate<Cut>(1, _pi[idx]));
    }

    // according to run-time parameters, choose mapping algorithm

    int num_pass_flow  = 3;
    int num_pass_exact = 1;

    for (int i = 0; i != num_pass_flow; ++i) {
        MappingPass pass(CutCostAlgo::FLOW, *this);
    }

    for (int i = 0; i != num_pass_exact; ++i) {
        MappingPass pass(CutCostAlgo::EXACT, *this);
    }

    return create_mapped_graph();
}


Abc_Ntk_t *PerformSupperMap(Abc_Ntk_t *pNtk, const Config &cfg)
{
auto t0 = std::clock();
    mapper *mgr = mapper::create_from_aig(static_cast<void *>(pNtk));
auto t1 = std::clock();
    if (!mgr)
        return nullptr;
    graph_t *mapped_graph = mgr->run_lut_mapping(cfg);
auto t2 = std::clock();
    Abc_Ntk_t *pNtkMapped = mapped_graph->to_ntk();
auto t3 = std::clock();
    delete mapped_graph;
    delete mgr;
    return pNtkMapped;
}

}
