#include <cstddef>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>
#include <ctime>
#include <print>
#include <map>

#include "base/abc/abc.h"

#include "basic.hpp"
#include "map.hpp"

using namespace abc;

namespace fox::supper {
std::string
Cut::operator*() const
{
    std::string res; res.reserve(20);
    res = "{ ";
    for (int i = 0; i != size; ++i) {
        res += std::to_string(leaves[i]) + " ";
    }
    res += "}";
    return res;
}

bool
graph_t::is_topologically_sorted() const
{
    // For a logic node, all of its fanins should be stored before it.
    ForEachGraphLogicNode(*this) {
        const auto &node = _nodes[idx];
        for (int i = 0; i != node.size(); ++i)
            if (node[i].id() > idx)
                return false;
    }
    return true;
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

void *
graph_t::to_abc_ntk()
{
    Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    if (!pNtk)
        return nullptr;

    return static_cast<void *>(pNtk);
}

void
MappingPass::improve_mapping_exactly(mapper &mgr)
{
    TIME_START(T)
    ForEachGraphLogicNode(mgr)
    {
        Cut *best_cut = mgr.best_cut(idx);
        if (mgr.num_est_ref(idx))
            mgr.rip_mffc(best_cut);
        CutCost best_cost(mgr.rip_mffc(best_cut), mgr.ref_mffc(best_cut));
        auto cut_set = mgr.cut_set(idx);
        for (int k = 0; k != cut_set.size() - 1; ++k)
        {
            Cut *cut = cut_set[k];
            CutCost cost(mgr.rip_mffc(cut), mgr.ref_mffc(cut));
            if (mgr.compare(best_cost, cost) == CutCost::cmp_res::RWIN)
            {
                best_cut = cut;
                best_cost = cost;
            }
        }

        if (mgr.num_est_ref(idx))
            mgr.ref_mffc(best_cut);

        if (best_cut != mgr.best_cut(idx)) {
            *mgr.best_cut(idx) = *best_cut;
        }
    }

    mgr.num_area() = 0;
    mgr.num_edge() = 0;
    ForEachGraphLogicNode(mgr) {
        if (mgr.num_est_ref(idx)) {
            mgr.num_area() ++;
            mgr.num_edge() += mgr.best_cut(idx)->size;
        }
    }

    if (mgr.config().verbose) {
        TIME_STOP(T)
        std::println(std::cout, "Ex LUT {}\tEdge {}\t Time {}", mgr.num_area(), mgr.num_edge(), Timer::formatted_time(cpu_T, 5));
    }
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
        std::cout << "  cut " << **cut << "\n";
    }
    std::cout << " best cut " << **_best_cut[id] << "\n";
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

    mgr->timer().start("create_graph");

    for (int n = 0; n != pNtk->vObjs->nSize; ++n) {
        Abc_Obj_t *pObj = Abc_NtkObj(pNtk, n);
        if (!pObj) [[unlikely]] {
            mgr->_nodes.emplace_back(graph_t::node_type_t::NONE);
            continue;
        }
        switch (pObj->Type) {
            case ABC_OBJ_CONST1:
                assert(mgr->_nodes.size() == 0);
                mgr->_nodes.emplace_back(graph_t::node_type_t::ONE);
                break;
            case ABC_OBJ_PI:
                mgr->_nodes.emplace_back(graph_t::node_type_t::PI);
                mgr->_pi.push_back(mgr->_nodes.size() - 1);
                mgr->_pi_names.push_back(Abc_ObjName(pObj));
                break;
            case ABC_OBJ_PO:
                mgr->_nodes.emplace_back(graph_t::node_type_t::PO, Lit(Abc_ObjFaninId0(pObj), pObj->fCompl0));
                mgr->_po.push_back(mgr->_nodes.size() - 1);
                mgr->_po_names.push_back(Abc_ObjName(pObj));
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

    mgr->timer().stop("create_graph");

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

word
mapper::compute_truth(Cut *cut, uint root) const
{
    static constexpr word init_val[6] = {
        0xAAAAAAAAAAAAAAAA,
        0xCCCCCCCCCCCCCCCC,
        0xF0F0F0F0F0F0F0F0,
        0xFF00FF00FF00FF00,
        0xFFFF0000FFFF0000,
        0xFFFFFFFF00000000
    };

    _timer.start("compute_truth");

    if (cut->size == 2 && _nodes[root][0].id() == cut->leaves[0] && _nodes[root][1].id() == cut->leaves[1]) {
        word t0 = init_val[0];
        word t1 = init_val[1];
        if (_nodes[root][0].sign())
            t0 = ~t0;
        if (_nodes[root][1].sign())
            t1 = ~t1;
        return t0 & t1;
    }

    std::map<uint, word> cache;

    const uint min_id = cut->leaves[0];
    ForEachCutLeaf(cut) {
        cache[leaf - min_id] = init_val[i];
    }

    std::function<void(uint)> fn = [&](uint n) {
        const auto &node = _nodes[n];
        assert(cache[n - min_id] == 0);
        assert(node.size() == 2);
        if (cache[node[0].id() - min_id] == 0)
            fn(node[0].id());
        if (cache[node[1].id() - min_id] == 0)
            fn(node[1].id());
        word t0 = cache[node[0].id() - min_id];
        word t1 = cache[node[1].id() - min_id];
        if (node[0].sign())
            t0 = ~t0;
        if (node[1].sign())
            t1 = ~t1;
        cache[n - min_id] = t0 & t1;
    };

    fn(root);

    _timer.stop("compute_truth");

    return cache[root - min_id];
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
        area += ref_mffc(best_cut(id));
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

void *
mapper::create_abc_ntk_from_mapping(bool use_truth_table)
{
    Abc_Ntk_t *ntk = use_truth_table ? Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1) : Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_AIG, 1);
    std::vector<Abc_Obj_t *> idx2obj(num_nodes(), nullptr);

    ForEachGraphPi(*this) {
        auto pi = Abc_NtkCreatePi(ntk);
        Abc_ObjAssignName(pi, const_cast<char *>(get_pi_name(idx).c_str()), nullptr);
        idx2obj[_pi[idx]] = pi;
    }
    ForEachGraphPo(*this) {
        auto po = Abc_NtkCreatePo(ntk);
        Abc_ObjAssignName(po, const_cast<char *>(get_po_name(idx).c_str()), nullptr);
        idx2obj[_po[idx]] = po;
    }

    auto create_sop_node = [&](uint idx, Abc_Obj_t *pConst1) {
        Cut *cut = best_cut(idx);
        word truth = compute_truth(cut, idx);
        Abc_Obj_t *pLut = Abc_NtkCreateObj(ntk, ABC_OBJ_NODE);
        if (truth == 0ul || truth == ~0ul) [[unlikely]] {
            Abc_ObjAddFanin(pLut, pConst1);
            if (truth == 0ul)
                pLut->pData = Abc_SopCreateBuf((Mem_Flex_t *)ntk->pManFunc);
            else
                pLut->pData = Abc_SopCreateInv((Mem_Flex_t *)ntk->pManFunc);
        } else {
            pLut->pData = Abc_SopRegister((Mem_Flex_t*)ntk->pManFunc, Abc_SopCreateFromTruth((Mem_Flex_t *)ntk->pManFunc,
                cut->size, (unsigned *)&truth));
            for (int m = 0; m != cut->size; ++m)
                Abc_ObjAddFanin(pLut, idx2obj[cut->leaves[m]]);
        }
        return pLut;
    };

    if (use_truth_table) {
        Abc_Obj_t *pConst1 = Abc_NtkCreateNodeConst1(ntk);
        // Hook the pwr
        idx2obj[pwr()] = pConst1;

        ForEachGraphLut(*this) {
            idx2obj[idx] = create_sop_node(idx, pConst1);
        }

        ForEachGraphPoV(*this) {
            const node_t &po = get_po(idx);
            if (po[0].sign()) {
                Abc_Obj_t *pInv = Abc_NtkCreateNodeInv(ntk, idx2obj[po[0].id()]);
                Abc_ObjAddFanin(idx2obj[po_id(idx)], pInv);
            } else {
                Abc_ObjAddFanin(idx2obj[po_id(idx)], idx2obj[po[0].id()]);
            }
        }

        // remove it if not used
        if (Abc_ObjFanoutNum(pConst1) == 0)
            Abc_NtkDeleteObj(pConst1);
    } else {

    }

    return static_cast<void *>(ntk);
}

graph_t *
mapper::run_lut_mapping(const Config &cfg)
{
    _cfg = cfg;

    _rank_fn = CutCost::GetRankFn(0);

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
    TIME_START(ALL);

    ///////////////////////////////////////
    mapper *mgr = mapper::create_from_aig(static_cast<void *>(pNtk));
    ///////////////////////////////////////

    if (!mgr) [[unlikely]] {
        std::println(std::cout, "SuperMap: failed to create mapper.");
        return nullptr;
    }

    if (cfg.verbose) {
        std::println(std::cout, ">>> SuperMap Started");
        std::println(std::cout, "    graph info: PI = {}, PO = {}, LOGIC = {}", mgr->num_pi(), mgr->num_po(), mgr->num_logic());
    }

    mgr->timer().start("lut_mapping");
    ///////////////////////////////////////
    graph_t *g = mgr->run_lut_mapping(cfg);
    ///////////////////////////////////////
    mgr->timer().stop ("lut_mapping");

    mgr->timer().start("create_abc_ntk");
    ///////////////////////////////////////
    Abc_Ntk_t *res_ntk = static_cast<Abc_Ntk_t *>(g ? g->to_abc_ntk() : mgr->create_abc_ntk_from_mapping());
    ///////////////////////////////////////
    mgr->timer().stop ("create_abc_ntk");

    TIME_STOP(ALL);

    if (cfg.verbose) {
        std::println(std::cout, ">>> Runtime Report");
        std::println(std::cout, "  Total CPU  Time {}  ", Timer::formatted_time(cpu_ALL,  5));
        std::println(std::cout, "  Total Wall Time {}\n", Timer::formatted_time(wall_ALL, 5));
        mgr->timer().report(std::cout);
    }

    ///////////////////////////////////////
    delete g;
    delete mgr;
    ///////////////////////////////////////

    return res_ntk;
}

}
