#include <vector>
#include <ctime>

#include "base/abc/abc.h"

#include "basic.hpp"
#include "map.hpp"

// using namespace abc;

namespace fox::supper {
std::string
Cut::to_str() const
{
    std::string res = "{ ";
    for (int i = 0; i != size; ++i) {
        res += std::to_string(leaves[i]) + " ";
    }
    res += "}";
    return res;
}

void
graph_t::report(std::ostream &os)
{
    os << "graph stats: ";
    os << "PI "    << num_pi()    << "\t";
    os << "PO "    << num_po()    << "\t";
    os << "LOGIC " << num_logic() << "\t";
    os << "\n";
}

Abc_Ntk_t *
graph_t::to_ntk()
{
    Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    if (!pNtk)
        return nullptr;

    return pNtk;
}

void
mapper::print_node(uint id)
{
    std::cout << "node id : " << id << "\n";
    std::cout << " type " << (uint)_nodes[id].type() << "\n";
    std::cout << " size " << _nodes[id].size() << "\n";
    for (int i = 0; i != _nodes[id].size(); ++i) {
        std::cout << "  fanin " << i << " : " << _nodes[id][i].val() << "\n";
    }
    std::cout << " cuts\n";
    for (const auto &cut : _cuts[id]) {
        std::cout << "  cut " << cut->to_str() << "\n";
    }
}

mapper *
mapper::create_from_aig(void *ntk)
{
    Abc_Ntk_t *pNtk = static_cast<Abc_Ntk_t *>(ntk);
    if (pNtk->ntkFunc != ABC_FUNC_AIG || pNtk->ntkType != ABC_NTK_STRASH) {
        std::cout << "unsupported ntk type\n";
        return nullptr;
    }

    mapper *mgr = new mapper(Abc_NtkObjNumMax(pNtk), Abc_NtkPiNum(pNtk), Abc_NtkPoNum(pNtk));

    for (int n = 0; n != pNtk->vObjs->nSize; ++n) {
        Abc_Obj_t *pObj = Abc_NtkObj(pNtk, n);
        if (!pObj) [[unlikely]] {
            mgr->_nodes.emplace_back(graph_t::node_type_t::NONE);
            continue;
        }
        switch (pObj->Type) {
            case ABC_OBJ_CONST1:
                mgr->_nodes.emplace_back(graph_t::node_type_t::ONE);
                break;
            case ABC_OBJ_PI:
                mgr->_nodes.emplace_back(graph_t::node_type_t::PI);
                mgr->_pi.push_back(mgr->_nodes.size() - 1);
                break;
            case ABC_OBJ_PO:
                mgr->_nodes.emplace_back(graph_t::node_type_t::PO, Lit(Abc_ObjFaninId0(pObj), pObj->fCompl0));
                mgr->_po.push_back(mgr->_nodes.size() - 1);
                break;
            case ABC_OBJ_NODE: [[likely]]
                mgr->_nodes.emplace_back(graph_t::node_type_t::LOGIC,
                    Lit(Abc_ObjFaninId0(pObj), pObj->fCompl0), Lit(Abc_ObjFaninId1(pObj), pObj->fCompl1));
                break;
            default:
                assert(0 && "unknown abc object type");
                mgr->_nodes.emplace_back(graph_t::node_type_t::NONE);
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
            ++_int_ref[n[k]];
    }

    for (int i = 0; i != _est_ref.size(); ++i)
        _est_ref[i] = _int_ref[i];
}

Area
mapper::ref_mffc(Cut *cut)
{
    Area area = 1.0;
    for (int i = 0; i != cut->size; ++i)
    {
        const uint id = cut->leaves[i];
        if (num_est_ref(id)++ > 0 || !_nodes[id].is_logic())
            continue;
        area += rip_mffc(best_cut(id));
    }
    return area;
}

Edge
mapper::rip_mffc(Cut *cut)
{
    Edge edge = cut->size;
    for (int i = 0; i != cut->size; ++i)
    {
        const uint id = cut->leaves[i];
        assert(num_est_ref(id) > 0);
        if (--num_est_ref(id) > 0 || !_nodes[id].is_logic())
            continue;
        edge += rip_mffc(best_cut(id));
    }
    return edge;
}

graph_t *
mapper::create_mapped_graph()
{
    graph_t *mapped = nullptr;

    return mapped;
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
        MappingPass(CutCostAlgo::FLOW, *this, i);
    }

    // for (int i = 0; i != num_pass_exact; ++i) {
    //     MappingPass(CutCostAlgo::EXACT, *this, i);
    // }

    return create_mapped_graph();
}


Abc_Ntk_t *PerformSupperMap(Abc_Ntk_t *pNtk, const Config &cfg)
{
auto t0 = std::clock();
    mapper *mgr = mapper::create_from_aig(static_cast<void *>(pNtk));
    if (!mgr)
        return nullptr;
    if (cfg.verbose) {
        std::cout << "created mapping graph with " << mgr->num_nodes() << " nodes. (PI = "
                  << mgr->num_pi() << ", PO = " << mgr->num_po() << ", LOGIC = " << mgr->num_logic() << ")\n";
    }
auto t1 = std::clock();
    graph_t *mapped_graph = mgr->run_lut_mapping(cfg);
auto t2 = std::clock();
    Abc_Ntk_t *pNtkMapped = mapped_graph->to_ntk();
auto t3 = std::clock();
    delete mapped_graph;
    delete mgr;
    return pNtkMapped;
}

}
