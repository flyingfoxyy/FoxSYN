#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <ctime>
#include <print>
#include <map>

#include "base/abc/abc.h"

#include "basic.hpp"
#include "cut.hpp"
#include "macros.hpp"
#include "prune.hpp"

#include "map.hpp"

using namespace abc;

namespace fox::supper {
mapper::mapper(uint max_node_num, uint num_pi, uint num_po) : graph_t(max_node_num, num_pi, num_po)
{
    constexpr uint kMax = std::numeric_limits<uint>::max() / 2;
    if (max_node_num > kMax) [[unlikely]] {
        std::println("Node number exceeds maximum limit {}, quit.", kMax);
        std::exit(1);
    }

    _int_ref .resize(max_node_num, 0);
    _est_ref .resize(max_node_num, 0);
    _area    .resize(max_node_num, 0);
    _edge    .resize(max_node_num, 0);
    _arrival .resize(max_node_num, 0);
    _required.resize(max_node_num, kMaxTime);
    _cuts    .resize(max_node_num, {});
    _is_lut_root.resize(max_node_num, 0);

    // Agdmap related initialization
    _id_counter = VID;
    _est_ref_agd.set_offset(VID);
    _is_lut_root_agd.set_offset(VID);
    _virtual_nodes.set_offset(VID);
}

mapper::~mapper()
{
    if (run_agdmap()) {
        ForEachGraphLogicNode(*this) {
            auto &cut_set = _cuts[idx];
            for (Cut *cut : cut_set) {
                Cut::dealloc(cut);
            }
        }

        for (Gate *gate : _gates) {
            if (gate && gate != (Gate *)01)
                delete gate;
        }
    }

    for (Cut *cut : _best_cuts) {
        Cut::dealloc(cut);
    }

    for (Cut *cut : _snapshot_best_cuts) {
        Cut::dealloc(cut);
    }
}

void
mapper::free_cuts() {
    ForEachGraphLogicNode(*this) {
        for (Cut *cut : _cuts[idx]) {
            Cut::dealloc(cut);
        }
        _cuts[idx].clear();
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
    std::cout << " best cut " << **best_cut(id) << "\n";
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
mapper::compute_truth(const Cut *cut, uint root) const
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

    if (cut->size == 2 && _nodes[root][0].id() == cut->leaf(0) && _nodes[root][1].id() == cut->leaf(1)) {
        word t0 = init_val[0];
        word t1 = init_val[1];
        if (_nodes[root][0].sign())
            t0 = ~t0;
        if (_nodes[root][1].sign())
            t1 = ~t1;
        return t0 & t1;
    }

    std::map<uint, word> cache;

    const uint min_id = cut->leaf(0);
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

CutCost
mapper::compute_cut_cost(CutCostAlgo algo, Cut *cut)
{
    CutCost cost;
    Time max_leaf_arr = 0;
    switch (algo) {
    case CutCostAlgo::FLOW: {
        cost.size = cut->size;
        // area-flow/edge-flow
        ForEachCutLeaf(cut) {
            cost.area += area(leaf);
            cost.edge += edge(leaf);
            max_leaf_arr = std::max(max_leaf_arr, arrival(leaf));
        }
        // TODO: using cost library
        if (_cfg.area_pass_mode) {
            cost.area /= std::max(1.0f, (float)cut->size);  // area-pass: normalize by cutsize
        } else {
            cost.area += 1.0;  // flow-pass: add 1 for the LUT itself
        }
        cost.edge += cut->size;
        cost.arr = max_leaf_arr + 1;
        break;
    }
    case CutCostAlgo::EXACT: {
        cost.size = cut->size;
        ForEachCutLeaf(cut) {
            max_leaf_arr = std::max(max_leaf_arr, arrival(leaf));
            if (!is_lut_root(leaf)) {
                cost.area += area(leaf);
                cost.edge += edge(leaf);
            }
        }
        cost.area += 1.0;
        cost.edge += cut->size;
        cost.arr = max_leaf_arr + 1;
        break;
    }
    default:
        break;
    }
    return cost;
}

graph_t *
mapper::create_mapped_graph()
{
    graph_t *mapped = new graph_t(num_pi() + num_po() + num_area() + 1, num_pi(), num_po());
    mapped->add_const1();

    std::vector<uint> node_map(num_nodes(), std::numeric_limits<uint>::max());
    std::vector<uint> virtual_map(num_virtual_nodes(), std::numeric_limits<uint>::max());
    node_map[0] = 0;

    ForEachGraphPi(*this) {
        node_map[pi_id(idx)] = mapped->add_pi(get_pi_name(idx));
    }
    ForEachGraphPo(*this) {
        mapped->add_po(get_po_name(idx));
    }

    std::function<uint(uint)> emit_lut = [&](uint id) -> uint {
        if (id >= VID) {
            const uint vidx = id - VID;
            Assert(vidx < virtual_map.size());
            if (virtual_map[vidx] != std::numeric_limits<uint>::max())
                return virtual_map[vidx];
        } else {
            Assert(id < node_map.size());
            if (node_map[id] != std::numeric_limits<uint>::max())
                return node_map[id];
            if (is_pi(id) || id == 0)
                return node_map[id];
        }

        const Cut *cut = best_cut(id);
        Assert(cut);

        std::vector<Lit> fanins;
        fanins.reserve(cut->size);
        ForEachCutLeaf(cut) {
            fanins.emplace_back(emit_lut(leaf));
        }

        const uint mapped_id = mapped->add_lut(std::move(fanins), cut->fid());
        if (id >= VID)
            virtual_map[id - VID] = mapped_id;
        else
            node_map[id] = mapped_id;
        return mapped_id;
    };

    ForEachGraphPo(*this) {
        const auto &po = get_po(idx);
        if (po.size() == 0) {
            continue;
        }
        uint mapped_fanin = emit_lut(po[0].id());
        mapped->set_po_fanin(idx, Lit(mapped_fanin, po[0].sign()));
    }

    return mapped;
}

static Abc_Obj_t *
create_lut_obj_rec(
    mapper      &mgr,
    Abc_Ntk_t   *ntk,
    uint         id,
    bool         use_cut_truth,
    Array<Abc_Obj_t *> &cache,
    Array<Abc_Obj_t *> &cache_virtual)
{
    if (id >= VID) {
        if (cache_virtual[id - VID]) {
            return cache_virtual[id - VID];
        }
    } else {
        if (cache[id]) {
            return cache[id];
        }
    }

    const Cut *cut  = mgr.best_cut(id); Assert(cut);
    Abc_Obj_t *pLut = Abc_NtkCreateObj(ntk, ABC_OBJ_NODE);

    if (id >= VID) {
        cache_virtual[id - VID] = pLut;
    } else {
        cache[id] = pLut;
    }

    word truth = 0ul;
    if (use_cut_truth)
        truth = cut->fid();
    else
        truth = mgr.compute_truth(cut, id);

    if (truth == 0ul || truth == ~0ul) [[unlikely]] {
        Abc_ObjAddFanin(pLut, cache[0]); // connect to constant 1
        if (truth == 0ul)
            pLut->pData = Abc_SopCreateBuf((Mem_Flex_t *)ntk->pManFunc);
        else
            pLut->pData = Abc_SopCreateInv((Mem_Flex_t *)ntk->pManFunc);
    } else {
        pLut->pData = Abc_SopRegister((Mem_Flex_t*)ntk->pManFunc, Abc_SopCreateFromTruth((Mem_Flex_t *)ntk->pManFunc,
            cut->size, (unsigned *)&truth));
        ForEachCutLeaf(cut) {
            Abc_Obj_t *pFanin = create_lut_obj_rec(mgr, ntk, leaf, use_cut_truth, cache, cache_virtual);
            Abc_ObjAddFanin(pLut, pFanin);
        }
    }

    return pLut;
}

void *
mapper::create_abc_ntk_from_mapping(bool use_truth_table, bool use_cut_truth)
{
    timer().start("create_abc_ntk");

    Abc_Ntk_t *ntk = use_truth_table ? Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1)
                                     : Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_AIG, 1);
    Array<Abc_Obj_t *> cache; cache.resize(num_nodes(), nullptr);
    Array<Abc_Obj_t *> cache_virtual; cache_virtual.resize(num_virtual_nodes(), nullptr);

    // Assign the port names
    // --
    ForEachGraphPi(*this) {
        auto pi = Abc_NtkCreatePi(ntk);
        Abc_ObjAssignName(pi, const_cast<char *>(get_pi_name(idx).c_str()), nullptr);
        cache[pi_id(idx)] = pi;
    }
    ForEachGraphPo(*this) {
        auto po = Abc_NtkCreatePo(ntk);
        Abc_ObjAssignName(po, const_cast<char *>(get_po_name(idx).c_str()), nullptr);
        cache[po_id(idx)] = po;
 }

    if (use_truth_table) {
        Abc_Obj_t *const1 = Abc_NtkCreateNodeConst1(ntk);
        cache[0] = const1;
        ForEachGraphPoV(*this) {
            Abc_Obj_t *lut = create_lut_obj_rec(*this, ntk, n[0].id(), use_cut_truth, cache, cache_virtual);
            Abc_ObjAddFanin(cache[po_id(idx)], lut);
            // Mark the PO fanin complemented instead of building an inverter node, which
            // would cost one node and one logic level. Matches graph_t::to_abc_ntk().
            if (n[0].sign()) {
                Abc_ObjSetFaninC(cache[po_id(idx)], 0);
            }
        }
        if (Abc_ObjFanoutNum(const1) == 0) {
            Abc_NtkDeleteObj(const1);
        }
    } else {
        Assert(0 && "Not Implemented");
    }

    timer().stop("create_abc_ntk");
    return static_cast<void *>(ntk);
}

void
mapper::dump_simple_gates(const char *fname)
{
    std::ofstream os(fname);
    os << "module top (";
    for (int i = 0; i != _pi_names.size(); ++i) {
        os << "n" << (_pi[i]) << ", ";
    }
    for (int i = 0; i != _po_names.size(); ++i) {
        os << "n" << (_po[i]) << ", ";
    }
    os << ");\n";
    os << "input ";
    for (int i = 0; i != _pi_names.size(); ++i) {
        os << "n" << (_pi[i]);
        if (i == _pi_names.size() - 1) {
            os << ";\n";
        } else {
            os << ", ";
        }
    }
    os << "output ";
    for (int i = 0; i != _po_names.size(); ++i) {
        os << "n" << (_po[i]);
        if (i == _po_names.size() - 1) {
            os << ";\n";
        } else {
            os << ", ";
        }
    }

    os << "wire ";
    uint id = 0;
    bool first = true;
    for (Gate *g : _gates) {
        if (g && g != (Gate *)01) {
            if (first) {
                os << "n" + std::to_string(id);
                first = false;
            } else {
                os << ", n" + std::to_string(id);
            }
        }
        ++id;
    }
    os << ";\n";

    id = 0;
    for (Gate *g : _gates) {
        if (g && g != (Gate *)01) {
            const auto &inputs = g->inputs();
            os << "assign n" << id << " = ";
            bool first = true;
            for (Lit lit : inputs) {
                if (!first) {
                    os << " & ";
                }
                first = false;
                if (lit.sign()) {
                    os << "~";
                }
                os << "n" << lit.id() << " ";
            }
            os << ";\n";
        }
        ++id;
    }

    for (int i = 0; i != _po.size(); ++i) {
        os << "assign n" << (_po[i]) << " = ";
        const auto &n = _nodes[_po[i]];
        if (n[0].sign()) {
            os << "~";
        }
        os << "n" << n[0].id() << ";\n";
    }
    os << "endmodule\n";
    os.close();
}

void
mapper::create_simple_gates(uint max_size)
{
    mapper &mgr = *this;

    _timer.start("create_gate");
    _gates.resize(num_nodes(), nullptr);

    // Mark the PI    
    std::fill(_gates.begin(), _gates.begin() + mgr.num_pi() + 1, (Gate *)01);

    // Mark the fanin of PO
    ForEachGraphPoV(mgr) {
        _gates[n[0]] = (Gate *)01;
    }

    // Mark the gate root (fanout > 1 or inverted input source node)
    ForEachGraphLogicNodeRev(mgr) {
        if (mgr.num_ref(idx) > 1) {
            _gates[idx] = (Gate *)01;
        }
    }

    auto expend = [&](this auto self, Lit fanin, std::vector<Lit> &internal)
    {
        if (internal.size() >= max_size - 1) {
            return;   
        }
        if (mgr._gates[fanin] || fanin.sign()) {
            return;
        }
        // It's ok to absorb fanin into current gate.
        internal.push_back(fanin);
        self(mgr[fanin][0], internal);
        self(mgr[fanin][1], internal);
    };

    std::vector<Lit> internal; internal.reserve(MAX_GATE_SIZE + 1);

    ForEachGraphLogicNodeRev(mgr) {
        if (_gates[idx] == (Gate *)01) {
            const auto &n = mgr[idx];
            internal.clear();
            internal.push_back(Lit(idx));
            expend(n[0], internal);
            expend(n[1], internal);

            // using push unique ?
            std::unordered_set<Lit> hash_set; hash_set.reserve(internal.size());
            for (Lit lit : internal) {
                hash_set.insert(lit);
            }
            std::vector<Lit> inputs; inputs.reserve(internal.size() + 1);
            for (Lit i : internal) {
                Assert(is_logic(i));
                Lit fanin0 = _nodes[i.id()][0];
                Lit fanin1 = _nodes[i.id()][1];
                if (!hash_set.contains(fanin0)) {
                    inputs.push_back(fanin0);
                    _gates[fanin0] = (Gate *)01;
                }
                if (!hash_set.contains(fanin1)) {
                    inputs.push_back(fanin1);
                    _gates[fanin1] = (Gate *)01;
                }
            }
            _gates[idx] = new Gate(std::move(inputs));
        }
    }

    if (config().verbose) {
        std::println(std::cout, "Simple gate stats:");
        uint num_gate[MAX_GATE_SIZE + 1] = {0};
        for (Gate *gate : _gates) {
            if (gate && gate != (Gate *)01) {
                ++num_gate[gate->size()];
            }
        }
        for (uint size = 2; size <= max_size; ++size) {
            if (uint number = num_gate[size]; number) {
                std::println(std::cout, "  gate size {} : {}", size, number);
            }
        }
    }

    _timer.stop("create_gate");
}

graph_t *
mapper::run_lut_mapping(const Config &cfg)
{
    timer().start("lut_mapping");

    _cfg     = cfg;
    _cfg.first_pass = true;
    _rank_fn = CutCost::GetRankFn(cfg.opt_target);

    // Setup PI cuts
    constexpr uint kTrivCutMemSize = Cut::bytes_needed<Cut::KCUT>(1);
    void *pi_triv_cuts = std::calloc(1, kTrivCutMemSize * num_pi());
    {
        char *p = reinterpret_cast<char *>(pi_triv_cuts);
        ForEachGraphPi(*this) {
            uint id         = _pi[idx];
            Cut *cut        = reinterpret_cast<Cut *>(p);
            cut->sign       = SIGNATURE(id);
            cut->size       = 1;
            cut->begin()[0] = id;
            cut->set_fid(0xAAAAAAAAAAAAAAAA);
            cut->area_cost  = 1.0f;
            _cuts[id].push_back(cut);
            p += kTrivCutMemSize;
        }
        Assert(p == (reinterpret_cast<char *>(pi_triv_cuts) + kTrivCutMemSize * num_pi()));
    }

    // Setup best cuts
    const uint kBestCutMemSize = Cut::bytes_needed<Cut::KCUT>(cfg.lut_size);
    Assert(_nodes[logic_begin()].is_logic());
    _best_cuts.set_offset(logic_begin());
    _best_cuts.resize(num_logic(), nullptr);
    ForEachGraphLogicNode(*this) {
        Cut *cut = (Cut *)std::calloc(1, kBestCutMemSize);
        cut->ms  = kBestCutMemSize;
        _best_cuts[idx] = cut;
    }

    auto clear_snapshot = [this]() {
        for (Cut *cut : _snapshot_best_cuts) {
            Cut::dealloc(cut);
        }
        _snapshot_best_cuts.clear();
        _snapshot_area = 0;
        _snapshot_edge = 0;
        _snapshot_delay = 0;
    };
    auto snapshot_best_cuts = [this, &clear_snapshot]() {
        clear_snapshot();
        _snapshot_best_cuts.reserve(num_logic());
        ForEachGraphLogicNode(*this) {
            _snapshot_best_cuts.push_back(Cut::copy(_best_cuts[idx]));
        }
        _snapshot_area = num_area();
        _snapshot_edge = num_edge();
        _snapshot_delay = num_delay();
    };
    auto restore_snapshot = [this]() {
        if (_snapshot_best_cuts.empty()) {
            return;
        }
        uint snap_idx = 0;
        ForEachGraphLogicNode(*this) {
            set_best_cut(idx, _snapshot_best_cuts[snap_idx++]);
        }
        num_area() = _snapshot_area;
        num_edge() = _snapshot_edge;
        num_delay() = _snapshot_delay;
    };

    clear_snapshot();

    // create simple gates boundary
    if (run_agdmap()) {
        create_simple_gates(8);
    }

    // according to run-time parameters, choose mapping algorithm
    uint prev_area = 0;
    uint prev_delay = 0;
    const CutCostAlgo iter_algo = CutCostAlgo::FLOW;
    {
        MappingPass pass(CutCostAlgo::FLOW, *this, 0);
        snapshot_best_cuts();
        prev_area = num_area();
        prev_delay = num_delay();
        update_fanout_estimation();
        // After initial pass, propagate required time for delay mode
        if (_cfg.delay_mode()) {
            reset_required();
            Backward bwd(*this);
            bwd.propagate_required();
        }
    }

    // --- Area-pass: use Σarea(leaf)/cutsize cost (agdmap skips this for delay mode) ---
    // For delay mode: run extra flow iterations (no ÷cutsize) to improve convergence.
    if (!run_agdmap()) {
        const bool use_area_cost = !_cfg.delay_mode();  // ÷cutsize only for area mode
        if (use_area_cost) _cfg.area_pass_mode = true;
        uint area_prev_area  = 0;
        uint area_prev_delay = 0;
        for (uint i = 0; i < _cfg.area_iter_num; ++i) {
            bool should_stop = false;
            {
                MappingPass pass(iter_algo, *this, -(int)i - 1);
                uint current_area  = num_area();
                uint current_delay = num_delay();
                bool better = _cfg.delay_mode() ?
                    (current_delay < _snapshot_delay ||
                     (current_delay == _snapshot_delay &&
                      (current_area < _snapshot_area ||
                       (current_area == _snapshot_area && num_edge() < _snapshot_edge)))) :
                    current_area < _snapshot_area;
                if (better) {
                    snapshot_best_cuts();
                }
                update_fanout_estimation();
                if (i == 0) {
                    // Seed within-area-pass convergence tracker from first area iteration.
                    area_prev_area  = current_area;
                    area_prev_delay = current_delay;
                } else {
                    if (_cfg.delay_mode()) {
                        // Stop only when neither delay nor area improves.
                        if (current_delay >= area_prev_delay && current_area >= area_prev_area) should_stop = true;
                    } else if (current_area >= area_prev_area) {
                        should_stop = true;
                    } else {
                        float improvement = area_prev_area == 0 ? 0.0f : float(area_prev_area - current_area) / float(area_prev_area);
                        if (improvement < _cfg.epsilon) should_stop = true;
                    }
                    area_prev_area  = current_area;
                    area_prev_delay = current_delay;
                }
            }
            if (should_stop) break;
        }
        if (use_area_cost) _cfg.area_pass_mode = false;
        // Reset prev_* to snapshot so flow-pass compares from here
        prev_area  = _snapshot_area;
        prev_delay = _snapshot_delay;
    }

    // Flow-pass: agdmap uses ratio=0.998, meaning stop when improvement < 0.2%.
    // The stop condition must compare flow iterations to each other (not vs the area-pass
    // best), mirroring agdmap itrSel where convergence is tracked within-pass.
    const float flow_epsilon = 0.0f;  // Stop only on no-improvement (matches agdmap itrSel exactly)
    uint flow_prev_area  = 0;
    uint flow_prev_delay = 0;
    for (uint i = 0; i < _cfg.area_iter_num; ++i) {
        bool should_stop = false;
        {
            MappingPass pass(iter_algo, *this, i);
            uint current_area = num_area();
            uint current_delay = num_delay();
            uint current_edge = num_edge();
            bool better = _cfg.delay_mode() ?
                (current_delay < _snapshot_delay ||
                 (current_delay == _snapshot_delay &&
                  (current_area < _snapshot_area ||
                   (current_area == _snapshot_area && current_edge < _snapshot_edge)))) :
                current_area < _snapshot_area;
            if (better) {
                snapshot_best_cuts();
            }
            update_fanout_estimation();
            if (i == 0) {
                // Seed the within-flow convergence tracker from the first flow iteration.
                flow_prev_area  = current_area;
                flow_prev_delay = current_delay;
            } else {
                if (_cfg.delay_mode()) {
                    // Stop only when neither delay nor area improves.
                    if (current_delay >= flow_prev_delay && current_area >= flow_prev_area) {
                        should_stop = true;
                    }
                } else if (current_area >= flow_prev_area) {
                    should_stop = true;
                } else {
                    float improvement = flow_prev_area == 0 ? 0.0f : float(flow_prev_area - current_area) / float(flow_prev_area);
                    if (improvement < flow_epsilon) {
                        should_stop = true;
                    }
                }
                flow_prev_area  = current_area;
                flow_prev_delay = current_delay;
            }
        }
        if (should_stop) {
            break;
        }
    }

    restore_snapshot();
    clear_snapshot();
    if (!run_agdmap()) {
        free_cuts();
    }
    std::free(pi_triv_cuts);

    graph_t *mapped_g = create_mapped_graph();

    timer().stop("lut_mapping");
    return mapped_g;
}

void
mapper::update_fanout_estimation()
{
    ForEachGraphLogicNode(*this) {
        float actual_ref = num_est_ref(idx);
        if (is_lut_root(idx)) {
            num_est_ref(idx) = std::max(1.0f, actual_ref);
        } else {
            num_est_ref(idx) = 1.0f;
        }
    }
}

Time
mapper::calculate_delay()
{
    Time max_arr = 0;
    ForEachGraphPo(*this) {
        const auto &po = get_po(idx);
        if (po.size() == 0)
            continue;
        max_arr = std::max(max_arr, arrival(po[0].id()));
    }
    return max_arr;
}

template <CutCostAlgo algo> void
CutEnumerator<algo>::assign_node_id(std::vector<Cut *> &kcuts) {
    uint num_virtual = 0;
    for (Cut *kcut : kcuts) {
        if (kcut->head) {
            num_virtual += kcut->idx; // overwrite idx later.
        }
    }

    uint id_start = _mgr.fetch_free_id(num_virtual);
    uint cnt = 0;

    for (Cut *kcut : kcuts) {
        if (!kcut->head)
            continue;
        char *base = reinterpret_cast<char *>(kcut);
        char *end  = base + kcut->ms;
        uint *root_ids = kcut->get_root_ids();
        uint  nchain   = kcut->num_chain_cuts();
        std::function<void(Cut *)> visit_cut = [&](Cut *cut) -> void {
            ForEachCutLeaf(cut) {
                if (leaf >= AGD_MAX_ID) { // this is a leaf created in decomposition.
                    char *raw_cut  = base + (leaf - AGD_MAX_ID);
                    Cut  *leaf_cut = reinterpret_cast<Cut *>(raw_cut);
                    Assert(raw_cut < end && raw_cut >= base);
                    Assert(raw_cut + leaf_cut->num_bytes() <= end);
                    visit_cut(leaf_cut);
                    // Fetch new id after visiting children. So that the ids are in increasing order.
                    uint new_id = id_start + cnt++;
                    cut->change_leaf(i, new_id);
                    // Resolve the same temporary ID in root_ids array
                    for (uint ri = 0; ri < nchain; ++ri) {
                        if (root_ids[ri] == leaf) {
                            root_ids[ri] = new_id;
                            break;
                        }
                    }
                    // Register new_id's cut/area/edge info
                    CutCost cost = _mgr.compute_cut_cost(algo, leaf_cut);
                    _mgr.register_virtual_cut (new_id, leaf_cut);
                    _mgr.register_virtual_area(new_id, cost.area);
                    _mgr.register_virtual_edge(new_id, cost.edge);
                    _mgr.register_virtual_arrival(new_id, cost.arr);
                }
            }
            // Verify leaves order
            if constexpr (kDebugBuild) {
                uint prev_leaf = 0;
                ForEachCutLeaf(cut) {
                    Assert(leaf >= prev_leaf);
                    prev_leaf = leaf;
                }
            }
        };
        visit_cut(kcut);
    }

    Assert(num_virtual == cnt);
}

template <CutCostAlgo algo> void
CutEnumerator<algo>::post_enum(uint id, const CutCost &best_cost) {
    std::vector<Cut *> &cut_set = _mgr.cut_set(id);

    // Set the idx
    uint idx = 0;
    for (Cut *cut : cut_set) {
        cut->idx = idx++;
    }

    // Save the best cut
    _mgr.set_best_cut(id, cut_set.front());

    // Set the node area/edge/arr info
    const float ratio = 1.0 / std::max(1.0f, float(_mgr.num_est_ref(id)));
    if (!_mgr.run_agdmap() && _mgr.config().area_mode()) {
        // AREA mode: propagate agdmap paper area_cost from the min-area cut
        _mgr.area(id) = cut_set.front()->area_cost * ratio;
    } else {
        _mgr.area(id) = best_cost.area * ratio;
    }
    _mgr.edge(id) = best_cost.edge * ratio;
    _mgr.arrival(id) = best_cost.arr;

    // Statics
    _mgr.num_stored() += cut_set.size();

    // create trivial cut
    Cut *triv_cut = Cut::alloc_triv(id);
    triv_cut->idx = cut_set.size();
    triv_cut->set_fid(0xAAAAAAAAAAAAAAAA);
    // Trivial cut area_cost = min-area cut's area_cost + 1 (agdmap trivCutGen)
    if (!_mgr.run_agdmap()) {
        triv_cut->area_cost = cut_set.front()->area_cost + 1.0f;
    }
    cut_set.push_back(triv_cut);
}

template <CutCostAlgo algo> void
CutEnumerator<algo>::prune_kcut(std::vector<Cut *> &kcuts, std::vector<CutCost> &costs) {
    // Stage E: agdmap-style per-cutsize bucketed pruning for DELAY mode.
    // Caps {0,1,1,2,2,2,3} for LUT6; collect keeps ALL cuts (no area filter).
    if (!_mgr.run_agdmap() && _mgr.config().delay_mode()) {
        const uint k = _mgr.config().cut_size; // typically 6

        // Per-cutsize bucket caps for delay mode.
        // agdmap uses {0,1,1,2,2,2,3} (tight). We use 4x for better QoR with reprioritize.
        // Total = 44 cuts max per node (more than max_cut_num=20 default, covers all good cuts).
        std::vector<uint> caps;
        if (k == 6) {
            caps = {0, 4, 4, 8, 8, 8, 12};
        } else if (k == 4) {
            caps = {0, 4, 16, 24, 24};
        } else if (k == 5) {
            caps = {0, 4, 4, 8, 16, 20};
        } else {
            // Generic fallback
            caps.assign(k + 1, 8);
            caps[0] = 0;
            if (k >= 1) caps[1] = 4;
        }

        // Build per-cutsize buckets sorted by RANK (arrival-first via CompareDelaySizeAreaEdge).
        float epsilon = _cfg.epsilon;
        auto delay_rank = [epsilon](const CutCost &lhs, const CutCost &rhs) -> bool {
            return CutCost::CompareDelaySizeAreaEdge(lhs, rhs, epsilon) == CutCost::cmp_res::LWIN;
        };

        std::vector<std::vector<uint>> buckets(k + 1);
        for (uint i = 0; i < kcuts.size(); ++i) {
            uint sz = kcuts[i]->size;
            if (sz > k) continue;
            buckets[sz].push_back(i);
        }

        for (uint sz = 1; sz <= k; ++sz) {
            auto &bkt = buckets[sz];
            if (bkt.empty()) continue;
            // Sort ascending by delay rank (best first)
            std::sort(bkt.begin(), bkt.end(), [&costs, &delay_rank](uint a, uint b) {
                return delay_rank(costs[a], costs[b]);
            });
            // Cap bucket
            uint cap = caps[sz];
            if (bkt.size() > cap) bkt.resize(cap);
        }

        // Collect: keep ALL cuts (area_oriented=false in agdmap = no area filter).
        // Sweep small-to-large sizes, each bucket reverse (worst→best).
        std::vector<uint> survivors;
        for (uint sz = 1; sz <= k; ++sz) {
            auto &bkt = buckets[sz];
            for (auto it = bkt.rbegin(); it != bkt.rend(); ++it) {
                survivors.push_back(*it);
            }
        }
        // Reverse so survivors[0] = best by delay rank
        std::reverse(survivors.begin(), survivors.end());

        // Re-sort survivors by delay rank so [0] is truly the best
        std::sort(survivors.begin(), survivors.end(), [&costs, &delay_rank](uint a, uint b) {
            return delay_rank(costs[a], costs[b]);
        });

        // Build new kcuts, dealloc dropped cuts.
        std::vector<Cut *> saved;
        saved.reserve(survivors.size());
        std::vector<bool> keep(kcuts.size(), false);
        for (uint idx : survivors) keep[idx] = true;
        for (uint idx : survivors) {
            saved.push_back(kcuts[idx]);
            kcuts[idx] = nullptr;
        }
        for (Cut *cut : kcuts) {
            Cut::dealloc(cut);
        }
        std::swap(kcuts, saved);

        if (!kcuts.empty()) {
            costs[0] = _mgr.compute_cut_cost(algo, kcuts[0]);
            costs[0].idx = 0;
        }
        return;
    }

    // Stage B: agdmap-style per-cutsize bucketed pruning + monotonic area collect
    // Only for the plain smap -a path (not -g / wide-cut / delay mode).
    if (!_mgr.run_agdmap() && _mgr.config().area_mode()) {
        const uint k = _mgr.config().cut_size; // typically 6

        // Per-cutsize bucket caps for area mode.
        // Index = cutsize (0..k). Matching agdmap pruner store-num-upper values.
        // For K=6: agdmap uses {0,0,3,4,4,6} for sizes 0-5; size-6 is uncapped
        // (keep a generous cap to avoid removing valid full-size cuts).
        std::vector<uint> caps;
        if (k == 6) {
            caps = {0, 0, 3, 4, 4, 6, 20};  // agdmap area-mode analogue: more cuts for reprioritize
        } else if (k == 4) {
            caps = {0, 0, 3, 4, 4};
        } else if (k == 5) {
            caps = {0, 0, 3, 4, 4, 6};
        } else {
            // Generic fallback: 3 cuts per size bucket
            caps.assign(k + 1, 3);
            caps[0] = 0;
            if (k >= 1) caps[1] = 0;
        }

        // Build per-cutsize buckets of cut indices, sorted ascending by area_cost.
        // Each bucket is capped at caps[sz].
        std::vector<std::vector<uint>> buckets(k + 1);
        for (uint i = 0; i < kcuts.size(); ++i) {
            uint sz = kcuts[i]->size;
            if (sz > k) continue;
            buckets[sz].push_back(i);
        }

        float min_area = std::numeric_limits<float>::max();
        for (uint sz = 1; sz <= k; ++sz) {
            auto &bkt = buckets[sz];
            if (bkt.empty()) continue;
            // Sort ascending by area_cost
            std::sort(bkt.begin(), bkt.end(), [&kcuts](uint a, uint b) {
                return kcuts[a]->area_cost < kcuts[b]->area_cost;
            });
            // Cap bucket
            uint cap = caps[sz];
            if (bkt.size() > cap) bkt.resize(cap);
            // Update global min
            if (!bkt.empty()) {
                min_area = std::min(min_area, kcuts[bkt.front()]->area_cost);
            }
        }

        float value_upper = min_area + 1.0f;

        // Collect: sweep small-to-large sizes, each bucket reverse (worst→best),
        // keep if area_cost <= value_upper AND strictly decreasing.
        // Mirroring agdmap pruner::collect(area_oriented=true).
        std::vector<uint> survivors;
        float last_area = std::numeric_limits<float>::max();
        for (uint sz = 1; sz <= k; ++sz) {
            auto &bkt = buckets[sz];
            for (auto it = bkt.rbegin(); it != bkt.rend(); ++it) {
                float ac = kcuts[*it]->area_cost;
                if (ac <= value_upper && (survivors.empty() || ac < last_area)) {
                    survivors.push_back(*it);
                    last_area = ac;
                }
            }
        }
        // Reverse so survivors[0] = min-area cut (agdmap reverses at end)
        std::reverse(survivors.begin(), survivors.end());

        // Build new kcuts, dealloc dropped cuts.
        std::vector<bool> keep(kcuts.size(), false);
        for (uint idx : survivors) keep[idx] = true;

        std::vector<Cut *> saved;
        saved.reserve(survivors.size());
        for (uint idx : survivors) {
            saved.push_back(kcuts[idx]);
            kcuts[idx] = nullptr;
        }
        for (Cut *cut : kcuts) {
            Cut::dealloc(cut);
        }
        std::swap(kcuts, saved);

        // Rebuild costs: costs[0] gets the best-cut cost; rest are not used by
        // post_enum (only costs[0] matters). Re-compute costs[0] from kcuts[0].
        if (!kcuts.empty()) {
            costs[0] = _mgr.compute_cut_cost(algo, kcuts[0]);
            costs[0].idx = 0;
        }
        return;
    }

    // Default path: delay mode, -g path, or any other mode — keep existing behavior.
    float epsilon = _cfg.epsilon;
    CutCost::rank_fn fn = CutCost::GetRankFn(_mgr.config().opt_target);
    auto fn_wrap = [epsilon, fn](const CutCost &lhs, const CutCost &rhs) -> bool {
        return fn(lhs, rhs, epsilon) == CutCost::cmp_res::LWIN;
    };

    if (costs.size() > _cfg.max_cut_num) {
        std::sort(costs.begin(), costs.end(), fn_wrap);
    } else {
        auto best_itr = std::min_element(costs.begin(), costs.end(), fn_wrap);
        std::iter_swap(costs.begin(), best_itr);
    }

    // Mapping cost ranking back to cuts
    const size_t num_saved = std::min((size_t)_cfg.max_cut_num, costs.size());
    std::vector<Cut *> saved; saved.reserve(num_saved);
    while (saved.size() < num_saved) {
        Cut *&cut = kcuts[costs[saved.size()].idx];
        saved.push_back(cut);
        cut = nullptr;
    }

    for (Cut *cut : kcuts) {
        Cut::dealloc(cut);
    }

    std::swap(kcuts, saved);
}

template <CutCostAlgo algo> void
CutEnumerator<algo>::enumerate_kcut(uint id) {
    const Config &cfg = _mgr.config();

    Lit f0 = _mgr[id][0];
    Lit f1 = _mgr[id][1];

    const std::vector<Cut *> &cuts0 = _mgr.cut_set(f0);
    const std::vector<Cut *> &cuts1 = _mgr.cut_set(f1);

    uint num_pair = cuts0.size() * cuts1.size();
    _mgr.num_merged() += num_pair;
    
    std::vector<Cut *> kcuts; kcuts.reserve(num_pair / 2);

    constexpr uint kPoolCutNum = 82;
    kcut_t cut_pool[kPoolCutNum];
    uint buffer[MAX_LUT_SIZE << 1];

    uint k  = cfg.cut_size;
    uint ix = 0;

    for (uint i0 = 0, num0 = cuts0.size(); i0 != num0; ++i0) { Cut *c0 = cuts0[i0];
    for (uint i1 = 0, num1 = cuts1.size(); i1 != num1; ++i1) { Cut *c1 = cuts1[i1];
        if (c0->size + c1->size > k && popcount(c0->sign | c1->sign) > k) {
            continue;
        }
        auto end = std::set_union(c0->begin(), c0->end(), c1->begin(), c1->end(), buffer);
        if (end - buffer > k) {
            continue;
        }
        Cut *cut = Cut::alloc_kcut(buffer, end, c0->sign | c1->sign,
                                   ix == kPoolCutNum ? nullptr : reinterpret_cast<Cut *>(&cut_pool[ix++]));
        cut->set_fid(Cut::compute_truth(cut, sign_cond(c0, f0.sign()), sign_cond(c1, f1.sign())));
        // Agdmap paper area-flow formula (persistent, computed once at merge time)
        cut->area_cost = (c0->area_cost - 1.0f) / (float)std::max(1u, _mgr.num_ref(f0.id()))
                       + (c1->area_cost - 1.0f) / (float)std::max(1u, _mgr.num_ref(f1.id()))
                       + 1.0f;
        kcuts.push_back(cut);
    }} // end merge cuts

    _mgr.num_k_feasible() += kcuts.size();

    // Structure-based pruning

    // Restore the best cut from previous pass
    // if (Cut *best = _mgr.best_cut(id); best) {
    //     cuts.push_back(best->clone());
    // }

    // Calculate the cut cost
    std::vector<CutCost> costs; costs.resize(kcuts.size());
    ix = 0;
    for (Cut *cut: kcuts) {
        CutCost &cost = costs[ix];
        cost = _mgr.compute_cut_cost(algo, cut);
        cost.idx = ix++;
    }

    // Cost-based cut pruning
    prune_kcut(kcuts, costs);

    // Write kcuts into cut_set of node[id]
    std::vector<Cut *> &cut_set = _mgr.cut_set(id);
    cut_set.reserve(kcuts.size() + 1); // +1 for trivial cut
    for (Cut *cut : kcuts) {
        cut_set.push_back(cut->ms ? cut : Cut::copy(cut));
    }

    // Create trivial cut, set the area/edge info for gate.
    post_enum(id, costs[0]);
}

// TODO: when gate size <= 8, using stack memory to allocate wide-cut.
template <CutCostAlgo algo> void
CutEnumerator<algo>::enumerate_wcut(uint id) {
    const Config &cfg = _mgr.config();
    const bool wide_cut_delay_diag = cfg.wide_cut_delay_diag_active();
    const uint N_max_partial = wide_cut_delay_diag ? 32 : 8;
    const uint N_max_full = wide_cut_delay_diag ? 16 : 4;

    std::vector<Cut *> last_kcuts;
    std::unordered_map<Cut *, Cut *> last_kcut_map;
    std::unordered_map<Cut *, CutCost> last_cost_map;

    auto wcut_leaf_arrival = [this](const Cut *cut) -> Time {
        Time arr = 0;
        ForEachCutLeaf(cut) {
            arr = std::max(arr, _mgr.arrival(leaf));
        }
        return arr;
    };

    std::function<bool(Cut *, Cut *)> cmp;
    if (cfg.opt_target == Config::target_t::AREA) {
        cmp = [](Cut *lhs, Cut *rhs) -> bool {
            return lhs->area() < rhs->area();
        };
    } else {
        cmp = [&, wcut_leaf_arrival](Cut *lhs, Cut *rhs) -> bool {
            const auto lhs_it = last_cost_map.find(lhs);
            const auto rhs_it = last_cost_map.find(rhs);
            const Time lhs_arr = lhs_it == last_cost_map.end() ? wcut_leaf_arrival(lhs) : lhs_it->second.arr;
            const Time rhs_arr = rhs_it == last_cost_map.end() ? wcut_leaf_arrival(rhs) : rhs_it->second.arr;
            if (lhs_arr != rhs_arr) {
                return lhs_arr < rhs_arr;
            }
            return lhs->area() < rhs->area();
        };
    }

    // Do not reorder gate inputs here: agd_decompose() assumes sub_cuts[i]
    // corresponds to gate->input(i), so this diagnostic widens caps only.

    Gate *gate = _mgr.gate(id);
    uint  sz   = gate->size();
    uint  idx  = 1;
    uint  buffer[Cut::MAX_CUT_SIZE];

    std::vector<Cut *> curr_cuts(_mgr.cut_set(gate->input(0)));
    Prune<Cut *, PMT::Separated> prune(std::move(cmp));
    extern Cut *agd_decompose(mapper &mgr, uint id, Cut *wcut, CutCost &cost);

    while (true) {
        const bool is_last_fanin = idx == sz - 1;
        prune.reset((idx + 1) * cfg.lut_size, is_last_fanin ? N_max_full : N_max_partial);
        const auto& in_cuts = _mgr.cut_set(gate->input(idx));
        std::vector<Cut *> step_wcuts;

        for (uint ii = 0; ii != curr_cuts.size(); ++ii) { Cut *c0 = curr_cuts[ii];
        for (uint mm = 0; mm != in_cuts  .size(); ++mm) { Cut *c1 = in_cuts[mm];
            uint *end  = std::set_union(c0->begin(), c0->end(), c1->begin(), c1->end(), buffer);
            uint  size = end - buffer;
            // compute the area-cost for pruning
            // Or, just adding their area into a sum ?
            Cut *wcut = Cut::alloc_wcut(buffer, end);
            Edge leaf_edge = 0;
            for (int i = 0; i != size; ++i) {
                wcut->area() += _mgr.area(buffer[i]);
                if (is_last_fanin) {
                    leaf_edge += _mgr.edge(buffer[i]);
                }
            }
            // store the sub-cuts info
            if (idx == 1) {
                Assert(!c0->is_wcut() && !c1->is_wcut());
                wcut->add_sub_cut(c0->idx);
                wcut->add_sub_cut(c1->idx);
            } else {
                Assert(c0->is_wcut() && !c1->is_wcut());
                for (int i = 0; i != c0->num_sub_cuts(); ++i) {
                    wcut->add_sub_cut(c0->get_sub_cut(i));
                }
                wcut->add_sub_cut(c1->idx);
            }

            if (is_last_fanin) {
                CutCost cost;
                Cut *kcut = agd_decompose(_mgr, id, wcut, cost);
                wcut->area() += cost.area;
                cost.area = wcut->area();
                cost.edge += leaf_edge;
                cost.size = kcut->size;
                wcut->size = kcut->size;
                last_kcuts.push_back(kcut);
                last_kcut_map[wcut] = kcut;
                last_cost_map[wcut] = cost;
            }

            step_wcuts.push_back(wcut);
            prune.insert(wcut);
        }} // end for-loop

        if (idx != 1) {
            // Todo: reuse the cuts here
            for (Cut *cut : curr_cuts)
                Cut::dealloc(cut);
        }
        std::vector<Cut *> next_cuts;
        prune.get(next_cuts, 0, cfg.opt_target == Config::target_t::AREA);
        std::unordered_set<Cut *> survived_wcuts(next_cuts.begin(), next_cuts.end());
        for (Cut *wcut : step_wcuts) {
            if (survived_wcuts.find(wcut) == survived_wcuts.end()) {
                Cut::dealloc(wcut);
            }
        }
        curr_cuts.clear();
        curr_cuts.swap(next_cuts);
        if (++idx == sz) {
            break;
        }
    } // end while

    // Structure-based pruning
    // ?

    // Now curr_cuts contains all the final survived wide cuts
    // Decomposing them into k-feasible cuts
    std::vector<Cut *>   kcuts; kcuts.reserve(curr_cuts.size());
    std::vector<CutCost> costs; costs.reserve(curr_cuts.size());
    std::unordered_set<Cut *> surviving_kcuts;
    surviving_kcuts.reserve(curr_cuts.size());

    // Cost-based cut pruning
    for (uint i = 0; i != curr_cuts.size(); ++i) {
        Cut *wcut = curr_cuts[i];
        auto kcut_it = last_kcut_map.find(wcut);
        auto cost_it = last_cost_map.find(wcut);
        Assert(kcut_it != last_kcut_map.end());
        Assert(cost_it != last_cost_map.end());
        Cut *kcut = kcut_it->second;
        CutCost cost = cost_it->second;
        surviving_kcuts.insert(kcut);
        kcuts.push_back(kcut);
        cost.idx  = costs.size();
        costs.push_back(cost);
        Cut::dealloc(wcut);
    }
    for (Cut *kcut : last_kcuts) {
        if (surviving_kcuts.find(kcut) == surviving_kcuts.end()) {
            Cut::dealloc(kcut);
        }
    }

    std::vector<Cut *>().swap(curr_cuts); // clear the wide cuts

    // Ranking and prune the k-cuts of wide-cuts
    prune_kcut(kcuts, costs);

    // Assign node ids for virtual cuts
    assign_node_id(kcuts);

    // Set root_ids[0] = gate node id for each cut-list
    for (Cut *kcut : kcuts) {
        if (kcut->head) {
            kcut->get_root_ids()[0] = id;
        }
    }

    // Set the signature for each cut
    for (Cut *cut : kcuts) {
        uint sign = 0;
        ForEachCutLeaf(cut) {
            sign |= SIGNATURE(leaf);
        }
        cut->sign = sign;
    }

    // Write kcuts into cut_set of node[id]
    std::vector<Cut *> &cut_set = _mgr.cut_set(id);
    cut_set.reserve(kcuts.size() + 1); // +1 for trivial cut
    cut_set.insert(cut_set.end(), kcuts.begin(), kcuts.end());

    // Create trivial cut, set the area/edge info for gate.
    post_enum(id, costs.front());
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

    ///////////////////////////////////////
    graph_t *g = mgr->run_lut_mapping(cfg);
    ///////////////////////////////////////

    ///////////////////////////////////////
    Abc_Ntk_t *res_ntk = nullptr;
    if (g) {
        res_ntk = static_cast<Abc_Ntk_t *>(g->to_abc_ntk());
    }
    if (!res_ntk) {
        res_ntk = static_cast<Abc_Ntk_t *>(mgr->create_abc_ntk_from_mapping());
    }
    ///////////////////////////////////////

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
