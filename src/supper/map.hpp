#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <forward_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <linux/limits.h>
#include <memory>
#include <ostream>
#include <sys/types.h>
#include <type_traits>
#include <utility>
#include <vector>
#include <cassert>
#include <functional>
#include <deque>
#include <chrono>
#include <print>
#include <atomic>

#include "basic.hpp"
#include "graph.hpp"
#include "cut.hpp"
#include "agdmap.hpp"
#include "macros.hpp"

namespace abc {
    typedef struct Abc_Ntk_t_ Abc_Ntk_t;
}

namespace fox::supper {
// ========================================================================
// Config for mapping
// ========================================================================
class Config {
    bool setup() {
        return true;
    }

public:
    enum target_t : uint8_t {
        AREA,
        DELAY,
        EDGE
    };

    enum map_impl_t : uint32_t {
        PRIORITY_CUTS = 0x1,
        AGDMAP        = 0x4,
        ACDMAP        = 0x8
    };

    // user controllable
    target_t     opt_target  {target_t::AREA};
    uint         map_impl    {(uint)map_impl_t::PRIORITY_CUTS};
    uint         cut_size    {6};
    uint         lut_size    {6};
    uint         gate_size   {8};
    uint         max_cut_num {20};
    bool         verbose     {true };
    uint         area_iter_num {8};
    bool         wide_cut_delay_diag {false};
    // internal
    bool         first_pass      {true};
    bool         enum_truth      {true};
    float        epsilon         {0.005};
    bool         area_pass_mode  {false};

    bool area_mode()  const { return opt_target == target_t::AREA;  }
    bool delay_mode() const { return opt_target == target_t::DELAY; }
    bool wide_cut_delay_diag_active() const {
        return wide_cut_delay_diag && delay_mode() && map_impl == map_impl_t::AGDMAP;
    }
};

// ========================================================================
// CutCost
// ========================================================================
struct CutCost {
    enum class cmp_res {
        LWIN = 0,
        RWIN = 1,
        SAME = 2
    };
    using rank_fn = std::function<cmp_res(const CutCost &, const CutCost &, float)>;

    Area   area {0};
    Edge   edge {0};
    Time   arr  {kMaxTime};
    uint16 size {0};
    uint16 idx  {0};

    CutCost() = default;
    CutCost(Edge e, Area a, Time t = kMaxTime) : area(a), edge(e), arr(t), size(0), idx(0) {}

    std::string operator*() const {
        std::string str; str.reserve(64);
        str += "Area "  + std::to_string(area) + ", ";
        str += "Edge "  + std::to_string(edge) + ", ";
        str += "Arr "   + std::to_string(arr)  + ", ";
        str += "Size "  + std::to_string(size) + ", ";
        str += "Index " + std::to_string(idx);
        return str;
    }

    static cmp_res CompareAreaEdge(const CutCost &lhs, const CutCost &rhs, float epsilon) {
        if (lhs.area + epsilon < rhs.area)  return cmp_res::LWIN;
        if (lhs.area - epsilon > rhs.area)  return cmp_res::RWIN;
        if (lhs.edge + epsilon < rhs.edge)  return cmp_res::LWIN;
        if (lhs.edge - epsilon > rhs.edge)  return cmp_res::RWIN;
        if (lhs.size < rhs.size)    return cmp_res::LWIN;
        if (lhs.size > rhs.size)    return cmp_res::RWIN;
        return cmp_res::SAME;
    }

    static cmp_res CompareDelaySizeAreaEdge(const CutCost &lhs, const CutCost &rhs, float epsilon) {
        if (lhs.arr < rhs.arr)    return cmp_res::LWIN;
        if (lhs.arr > rhs.arr)    return cmp_res::RWIN;
        if (lhs.size < rhs.size)  return cmp_res::LWIN;
        if (lhs.size > rhs.size)  return cmp_res::RWIN;
        if (lhs.area + epsilon < rhs.area)  return cmp_res::LWIN;
        if (lhs.area - epsilon > rhs.area)  return cmp_res::RWIN;
        if (lhs.edge + epsilon < rhs.edge)  return cmp_res::LWIN;
        if (lhs.edge - epsilon > rhs.edge)  return cmp_res::RWIN;
        return cmp_res::SAME;
    }

    static rank_fn GetRankFn(Config::target_t mode) {
        if (mode == Config::target_t::AREA) // area
            return CompareAreaEdge;
        else
            return CompareDelaySizeAreaEdge; // delay
    }
};

enum class CutCostAlgo {
    FLOW,
    EXACT
};

enum class heuristic_t {
    FLOW,
    EXACT
};

// ========================================================================
// mapper
// ========================================================================
class mapper : public graph_t {
    Config                      _cfg     ;
    Array<uint>                 _int_ref ;
    Array<float>                _est_ref ;
    Array<Area>                 _area    ;
    Array<Edge>                 _edge    ;
    Array<Time>                 _arrival ;
    Array<Time>                 _required;
    Array<std::vector<Cut *>>   _cuts    ; // TODO: Using a pointer
    Array<Cut *>                _best_cuts;
    Array<uint8>                _is_lut_root;

    CutCost::rank_fn            _rank_fn ;

    // -- Agdmap related
    Array<Gate *>               _gates;         // Simple gates
    Array<float>                _est_ref_agd;   // Estimated reference count for Agdmap
    Array<uint8>                _is_lut_root_agd;
    std::atomic<uint>           _id_counter;    // Id counter for Agdmap virtual tree nodes

    struct VirtualNodeInfo {
        Cut  *cut      {nullptr};
        Area  area     {0};
        Edge  edge     {0};
        Time  arrival  {0};
        Time  required {kMaxTime};
    };
    Array<VirtualNodeInfo>      _virtual_nodes; // Virtual metadata for Agdmap decomposition nodes

    mutable Timer _timer;

    uint _bc_size  {0};
    uint _num_area {0};
    uint _num_edge {0};
    uint _num_delay{0};
    std::vector<Cut *> _snapshot_best_cuts;
    uint _snapshot_area {0};
    uint _snapshot_edge {0};
    uint _snapshot_delay{0};

    uint64_t _stat_cut[3]{0};

public:
    friend class enumerate_cut;

    using CutSet = std::vector<Cut *>;

    mapper(uint max_node_num, uint num_pi = 0, uint num_po = 0);
    ~mapper();

    Config &config() { return _cfg; }

    void initialize();

    bool run_agdmap() const { return _cfg.map_impl == Config::AGDMAP; }

    uint num_virtual_nodes() const {
        return _id_counter.load() - VID;
    }

    // reset flags
    Inline void reset_est_ref () { std::fill(_est_ref .begin(), _est_ref .end(), 0       ); }
    Inline void reset_required() {
        std::fill(_required.begin(), _required.end(), kMaxTime);
        for (auto &info : _virtual_nodes) {
            info.required = kMaxTime;
        }
    }
    Inline void reset_is_lut_root() {
        std::fill(_is_lut_root.begin(), _is_lut_root.end(), 0);
        std::fill(_is_lut_root_agd.begin(), _is_lut_root_agd.end(), 0);
    }

    // creators
    static mapper *create_from_aig (void       *ntk );
    static mapper *create_from_gia (void       *gia );
    static mapper *create_from_blif(const char *blif);

    template<Indexable T>
    Inline bool is_logic(T n) {
        Assert(n < AGD_MAX_ID);
        return n >= VID || (n >= (uint)logic_begin() && n < logic_end()); // including virtual nodes
    }

    template<Indexable T>
    Inline bool is_pi(T n) {
        return n >= (uint)pi_begin() && n < (uint)pi_end();
    }

    template<Indexable T>
    Inline std::vector<Cut *> &cut_set(T n) {
        return _cuts[n];
    }

    template<Indexable T>
    Inline Area &area(T n) {
        Assert(is_logic(n) || is_pi(n));
        return n >= VID ? _virtual_nodes[n].area : _area[n];
    }

    template<Indexable T>
    Inline Edge &edge(T n) {
        Assert(is_logic(n) || is_pi(n));
        return n >= VID ? _virtual_nodes[n].edge : _edge[n];
    }

    template<Indexable T> Inline Time &arrival(T n) { return n >= VID ? _virtual_nodes[n].arrival : _arrival[n]; }
    template<Indexable T> Inline Time &required(T n) {
        if (n >= VID) {
            return _virtual_nodes[n].required;
        }
        return _required[n];
    }
    template<Indexable T> Inline float &num_est_ref(T n) { return n >= VID ? _est_ref_agd[n] : _est_ref[n];  }
    template<Indexable T> Inline uint  &num_ref    (T n) { return _int_ref[n];  }
    template<Indexable T> Inline bool is_lut_root(T n) const { return n >= VID ? _is_lut_root_agd[n] : _is_lut_root[n]; }
    template<Indexable T> Inline void set_is_lut_root(T n, bool v) { (n >= VID ? _is_lut_root_agd[n] : _is_lut_root[n]) = v ? 1 : 0; }

    template<Indexable T>
    Inline const Cut *best_cut(T n) {
        Assert(is_logic(n));
        return n >= VID ? _virtual_nodes[n].cut : _best_cuts[n];
    }

    template<Indexable T>
    void set_best_cut(T n, const Cut *cut) {
        if constexpr (kDebugBuild) {
            ForEachCutLeaf(cut) {
                Assert(is_pi(leaf) || is_logic(leaf));
            }
        }
        Assert(is_logic(n) && cut);
        if (Cut *&best = _best_cuts[n]; best) {
            if (best->ms < cut->num_bytes()) {
                Cut::dealloc(best);
                best = Cut::copy(cut);
            } else {
                *best = *cut; // copy
            }
        } else {
            best = Cut::copy(cut);
        }
    }

    Inline uint &num_area()  { return _num_area;  }
    Inline uint &num_edge()  { return _num_edge;  }
    Inline uint &num_delay() { return _num_delay; }

    Inline const std::string &get_pi_name(uint idx) const { return _pi_names[idx]; }
    Inline const std::string &get_po_name(uint idx) const { return _po_names[idx]; }

    uint64_t &num_merged    () { return _stat_cut[0]; }
    uint64_t &num_k_feasible() { return _stat_cut[1]; }
    uint64_t &num_stored    () { return _stat_cut[2]; }

    CutCost::cmp_res compare(const CutCost &lhs, const CutCost &rhs) {
        return _rank_fn(lhs, rhs, _cfg.epsilon);
    }

    CutCost::cmp_res compare_with_required(uint idx, const CutCost &lhs, const CutCost &rhs) {
        if (!_cfg.delay_mode()) {
            return compare(lhs, rhs);
        }

        if (lhs.arr == rhs.arr) {
            return CutCost::CompareAreaEdge(lhs, rhs, _cfg.epsilon);
        }

        Time req = required(idx);
        if (req == kMaxTime) {
            return compare(lhs, rhs);
        }

        const bool lhs_meets_req = lhs.arr <= req;
        const bool rhs_meets_req = rhs.arr <= req;
        if (lhs_meets_req != rhs_meets_req) {
            return lhs_meets_req ? CutCost::cmp_res::LWIN : CutCost::cmp_res::RWIN;
        }

        const bool lhs_has_slack = lhs.arr < req;
        const bool rhs_has_slack = rhs.arr < req;
        if (lhs_has_slack && rhs_has_slack) {
            CutCost::cmp_res area_res = CutCost::CompareAreaEdge(lhs, rhs, _cfg.epsilon);
            if (area_res != CutCost::cmp_res::SAME) {
                return area_res;
            }
        }

        return compare(lhs, rhs);
    }

    // mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm
    // Agdmap related functions
    void create_simple_gates(uint max_size);

    void dump_simple_gates(const char *fname);

    Inline Gate *gate(uint id) {
        return _gates[id];
    }

    Inline uint fetch_free_id(uint num) {
        uint id = _id_counter.fetch_add(num, std::memory_order_relaxed);
        if (num) {
            _virtual_nodes.resize(_id_counter.load(std::memory_order_relaxed) - VID);
        }
        return id;
    }

    Inline void setup_agdmap_containers() {
        _est_ref_agd.clear();
        _est_ref_agd.resize(_id_counter.load() - VID, 0);
        _is_lut_root_agd.clear();
        _is_lut_root_agd.resize(_id_counter.load() - VID, 0);
    }

    Inline void register_virtual_cut(uint id, Cut *cut) {
        if constexpr (kDebugBuild) {
            ForEachCutLeaf(cut) {
                Assert(is_pi(leaf) || is_logic(leaf));
            }
        }
        Assert(id >= VID && id < AGD_MAX_ID);
        Assert(cut->size <= AGD_MAX_LUT_SIZE);
        Assert(_virtual_nodes[id].cut == nullptr || _virtual_nodes[id].cut == cut);
        _virtual_nodes[id].cut = cut;
    }

    Inline void register_virtual_area(uint id, Area area) {
        Assert(id >= VID && id < AGD_MAX_ID);
        _virtual_nodes[id].area = area;
    }

    Inline void register_virtual_edge(uint id, Edge edge) {
        Assert(id >= VID && id < AGD_MAX_ID);
        _virtual_nodes[id].edge = edge;
    }

    Inline void register_virtual_arrival(uint id, Time arr) {
        Assert(id >= VID && id < AGD_MAX_ID);
        _virtual_nodes[id].arrival = arr;
    }

    Timer &timer() { return _timer; }

    graph_t *run_lut_mapping(const Config &cfg);
    void update_fanout_estimation();

    graph_t *create_mapped_graph();

    void *create_abc_ntk_from_mapping(bool use_truth_table = true, bool use_cut_truth = true);

    Time calculate_delay();

    void free_cuts();

    CutCost compute_cut_cost(CutCostAlgo algo, Cut *cut);

    word compute_truth(const Cut *cut, uint root) const;

    void print_node(uint id);
};

// ========================================================================
// Enumerator of cut
// ========================================================================
// normal enumerate cut way
template <CutCostAlgo algo>
class CutEnumerator {
    mapper       &_mgr;
    const Config &_cfg;

    void assign_node_id(std::vector<Cut *> &kcuts);

    void post_enum(uint id, const CutCost &best_cost);

    void prune_kcut(std::vector<Cut *> &kcuts, std::vector<CutCost> &costs);

    void enumerate_kcut(uint id);

    // TODO: when gate size <= 8, using stack memory to allocate wide-cut.
    void enumerate_wcut(uint id);

    using kcut_t = kCut<MAX_LUT_SIZE>;

public:
    CutEnumerator(mapper &mgr, uint id) : _mgr(mgr), _cfg(mgr.config())
    {
        if (mgr.run_agdmap()) {
            if (Gate *g = mgr.gate(id); g) {
                if (g->size() > 2) {
                    enumerate_wcut(id);
                } else {
                    enumerate_kcut(id);
                }
            }
        } else {
            enumerate_kcut(id);
        }
    }
};

inline CutCost recompute_stored_cut_cost(mapper &mgr, CutCostAlgo algo, const Cut *root_cut) {
    Cut *cut = const_cast<Cut *>(root_cut);
    if (!cut->head) {
        return mgr.compute_cut_cost(algo, cut);
    }

    std::vector<Cut *> chain;
    chain.reserve(10);
    Cut *cur = cut;
    do {
        chain.push_back(cur);
    } while ((cur = cur->next()));

    Assert(chain.back()->tail);
    uint *root_ids = reinterpret_cast<uint *>(
        reinterpret_cast<char *>(chain.back()) + chain.back()->num_bytes());

    CutCost root_cost;
    for (int j = static_cast<int>(chain.size()) - 1; j >= 0; --j) {
        CutCost cost = mgr.compute_cut_cost(algo, chain[j]);
        if (j > 0) {
            uint sub_root = root_ids[j];
            if (sub_root >= VID) {
                Assert(sub_root < AGD_MAX_ID);
                float ratio = 1.0f / std::max(1.0f, mgr.num_est_ref(sub_root));
                mgr.area(sub_root) = cost.area * ratio;
                mgr.edge(sub_root) = cost.edge * ratio;
                mgr.arrival(sub_root) = cost.arr;
            }
        } else {
            root_cost = cost;
        }
    }

    return root_cost;
}

// ========================================================================
// Forward visitor
// ========================================================================
class Forward {
protected:
    mapper &_mgr;
public:
    Forward(mapper &mgr) : _mgr(mgr) {}
    virtual ~Forward() = default;

    virtual void impl() = 0;

    void reprioritize(CutCostAlgo algo) {
        _mgr.timer().start("forward");

        auto recompute_cut_list_cost = [&](Cut *root_cut) -> CutCost {
            return recompute_stored_cut_cost(_mgr, algo, root_cut);
        };

        bool agdmap = _mgr.run_agdmap();
        ForEachGraphLogicNode(_mgr) {
            auto &cuts = _mgr.cut_set(idx);
            if (cuts.empty()) continue;

            if (agdmap && _mgr.gate(idx) && _mgr.gate(idx)->size() > 2) {
                uint best_idx = 0;
                CutCost best_cost = recompute_cut_list_cost(cuts[0]);
                for (uint i = 1; i < cuts.size() - 1; ++i) {
                    CutCost cost = recompute_cut_list_cost(cuts[i]);
                    if (_mgr.compare_with_required(idx, cost, best_cost) == CutCost::cmp_res::LWIN) {
                        best_cost = cost;
                        best_idx  = i;
                    }
                }
                float ratio = 1.0f / std::max(1.0f, _mgr.num_est_ref(idx));
                _mgr.area(idx) = best_cost.area * ratio;
                _mgr.edge(idx) = best_cost.edge * ratio;
                _mgr.arrival(idx) = best_cost.arr;
                _mgr.set_best_cut(idx, cuts[best_idx]);
            } else if (cuts.size()) {
                uint best_idx = 0;
                CutCost best_cost = _mgr.compute_cut_cost(algo, cuts[0]);
                for (uint i = 1; i < cuts.size() - 1; ++i) {
                    CutCost cost = _mgr.compute_cut_cost(algo, cuts[i]);
                    if (_mgr.compare_with_required(idx, cost, best_cost) == CutCost::cmp_res::LWIN) {
                        best_cost = cost;
                        best_idx  = i;
                    }
                }
                float ratio = 1.0f / std::max(1.0f, _mgr.num_est_ref(idx));
                if (!agdmap && _mgr.config().area_mode()) {
                    // AREA mode: propagate agdmap paper area_cost from the selected best cut
                    _mgr.area(idx) = cuts[best_idx]->area_cost * ratio;
                } else {
                    _mgr.area(idx) = best_cost.area * ratio;
                }
                _mgr.edge(idx) = best_cost.edge * ratio;
                _mgr.arrival(idx) = best_cost.arr;
                _mgr.set_best_cut(idx, cuts[best_idx]);
            }
        }

        _mgr.timer().stop("forward");
    }
};

class ForwardFlow : public Forward {
public:
    ForwardFlow(mapper &mgr) : Forward(mgr) {}

    virtual void impl() {
        _mgr.timer().start("forward");

        ForEachGraphLogicNode(_mgr) {
            CutEnumerator<CutCostAlgo::FLOW>(_mgr, idx);
        }

        _mgr.timer().stop("forward");
    }
};

class ForwardExact : public Forward {
public:
    ForwardExact(mapper &mgr) : Forward(mgr) {}

    virtual void impl() {
        ForEachGraphLogicNode(_mgr) {
            CutEnumerator<CutCostAlgo::EXACT>(_mgr, idx);
        }
    }
};

// generic backword pass
// visit nodes in reversed topological order and set the best cuts and propagate required times
// ========================================================================
// Backward visitor
// ========================================================================
class Backward {
protected:
    mapper &_mgr;

    virtual const Cut *get_best_cut(uint idx) {
        return _mgr.best_cut(idx);
    }

    void reference_cut_rec(uint idx) {
        _mgr.set_is_lut_root(idx, true);
        const Cut *cut = _mgr.best_cut(idx);
        ForEachCutLeaf(cut) {
            if (_mgr.num_est_ref(leaf)++ == 0 && _mgr.is_logic(leaf)) {
                reference_cut_rec(leaf);
            }
        }
        _mgr.num_area() ++;
        _mgr.num_edge() += cut->size;
    }

    virtual void reference_best_cuts() {
        _mgr.reset_is_lut_root();
        ForEachGraphPoV(_mgr) {
            uint po_fanin = _mgr.get_po(idx)[0].id();
            if (_mgr.num_est_ref(po_fanin)++ == 0 && _mgr.is_logic(po_fanin)) {
                reference_cut_rec(po_fanin);
            }
        }
    }

    void propagate_required_rec(uint idx) {
        const Cut *cut = get_best_cut(idx);
        Assert(cut);

        Time req = _mgr.required(idx);
        Time leaf_req = req > 0 ? req - 1 : 0;
        ForEachCutLeaf(cut) {
            Time &current_req = _mgr.required(leaf);
            if (leaf_req < current_req) {
                current_req = leaf_req;
                if (_mgr.is_logic(leaf)) {
                    propagate_required_rec(leaf);
                }
            }
        }
    }

public:
    void propagate_required() {
        Time required = _mgr.calculate_delay();
        ForEachGraphPo(_mgr) {
            const auto &po = _mgr.get_po(idx);
            if (po.size() == 0) {
                continue;
            }
            uint po_fanin = po[0].id();
            Time &current_req = _mgr.required(po_fanin);
            if (required < current_req) {
                current_req = required;
                if (_mgr.is_logic(po_fanin)) {
                    propagate_required_rec(po_fanin);
                }
            }
        }
    }

public:
    Backward(mapper &mgr) : _mgr(mgr) {}
    virtual ~Backward() = default;

    virtual void impl() {
        _mgr.timer().start("backward");
        _mgr.reset_est_ref();

        reference_best_cuts();

        if (_mgr.config().delay_mode()) {
            _mgr.reset_required();
            propagate_required();
        }
        _mgr.timer().stop("backward");
    }
};

class BackwardExact : public Backward {
    CutCost compute_exact_cost(const Cut *root_cut) {
        return recompute_stored_cut_cost(_mgr, CutCostAlgo::EXACT, root_cut);
    }

    const Cut *select_cut(uint idx, CutCost &best_cost) {
        if (idx >= VID) {
            const Cut *cut = _mgr.best_cut(idx);
            Assert(cut);
            best_cost = compute_exact_cost(cut);
            return cut;
        }

        auto &cuts = _mgr.cut_set(idx);
        Assert(!cuts.empty());

        const uint limit = cuts.size() > 1 ? cuts.size() - 1 : cuts.size();
        uint best_idx = 0;
        best_cost = compute_exact_cost(cuts[0]);
        for (uint i = 1; i < limit; ++i) {
            CutCost cost = compute_exact_cost(cuts[i]);
            if (_mgr.compare_with_required(idx, cost, best_cost) == CutCost::cmp_res::LWIN) {
                best_cost = cost;
                best_idx = i;
            }
        }

        _mgr.set_best_cut(idx, cuts[best_idx]);
        return _mgr.best_cut(idx);
    }

    void reference_selected_cut_rec(uint idx) {
        CutCost best_cost;
        const Cut *cut = select_cut(idx, best_cost);

        _mgr.set_is_lut_root(idx, true);
        // Keep per-root area/edge aligned with forward reprioritize:
        // downstream EXACT cost queries use cost / max(1, num_est_ref).
        float ratio = 1.0f / std::max(1.0f, _mgr.num_est_ref(idx));
        _mgr.area(idx) = best_cost.area * ratio;
        _mgr.edge(idx) = best_cost.edge * ratio;
        _mgr.arrival(idx) = best_cost.arr;

        ForEachCutLeaf(cut) {
            if (_mgr.num_est_ref(leaf)++ == 0 && _mgr.is_logic(leaf)) {
                reference_selected_cut_rec(leaf);
            }
        }

        _mgr.num_area()++;
        _mgr.num_edge() += cut->size;
    }

protected:
    void reference_best_cuts() override {
        _mgr.reset_is_lut_root();
        ForEachGraphPoV(_mgr) {
            uint po_fanin = _mgr.get_po(idx)[0].id();
            if (_mgr.num_est_ref(po_fanin)++ == 0 && _mgr.is_logic(po_fanin)) {
                reference_selected_cut_rec(po_fanin);
            }
        }
    }

public:
    BackwardExact(mapper &mgr) : Backward(mgr) {}
};

class BackwardFlowAgd : public Backward {
    CutCost compute_flow_cost(const Cut *root_cut) {
        return _mgr.compute_cut_cost(CutCostAlgo::FLOW, const_cast<Cut *>(root_cut));
    }

    const Cut *select_cut(uint idx, CutCost &best_cost) {
        if (idx >= VID) {
            const Cut *cut = _mgr.best_cut(idx);
            Assert(cut);
            best_cost = compute_flow_cost(cut);
            return cut;
        }

        auto &cuts = _mgr.cut_set(idx);
        Assert(cuts.size() > 1);

        const uint limit = cuts.size() - 1;
        uint best_idx = 0;
        best_cost = compute_flow_cost(cuts[0]);
        for (uint i = 1; i < limit; ++i) {
            CutCost cost = compute_flow_cost(cuts[i]);
            if (_mgr.compare_with_required(idx, cost, best_cost) == CutCost::cmp_res::LWIN) {
                best_cost = cost;
                best_idx = i;
            }
        }

        _mgr.set_best_cut(idx, cuts[best_idx]);
        return _mgr.best_cut(idx);
    }

    void add_root_reference(uint idx) {
        if (!_mgr.is_logic(idx)) {
            return;
        }
        if (_mgr.num_est_ref(idx)++ != 0) {
            return;
        }

        _mgr.area(idx) = 0;
        _mgr.edge(idx) = 0;
        if (idx >= VID) {
            reference_virtual_cut_rec(idx);
        }
    }

    void reference_virtual_cut_rec(uint idx) {
        Assert(idx >= VID);
        if (_mgr.is_lut_root(idx)) {
            return;
        }

        const Cut *cut = _mgr.best_cut(idx);
        Assert(cut);
        CutCost best_cost = compute_flow_cost(cut);

        _mgr.set_is_lut_root(idx, true);
        float ratio = 1.0f / std::max(1.0f, _mgr.num_est_ref(idx));
        _mgr.area(idx) = best_cost.area * ratio;
        _mgr.edge(idx) = best_cost.edge * ratio;
        _mgr.arrival(idx) = best_cost.arr;

        ForEachCutLeaf(cut) {
            add_root_reference(leaf);
        }

        _mgr.num_area()++;
        _mgr.num_edge() += cut->size;
    }

    void reference_selected_cut(uint idx) {
        CutCost best_cost;
        const Cut *cut = select_cut(idx, best_cost);

        _mgr.set_is_lut_root(idx, true);
        float ratio = 1.0f / std::max(1.0f, _mgr.num_est_ref(idx));
        _mgr.area(idx) = best_cost.area * ratio;
        _mgr.edge(idx) = best_cost.edge * ratio;
        _mgr.arrival(idx) = best_cost.arr;

        ForEachCutLeaf(cut) {
            add_root_reference(leaf);
        }

        _mgr.num_area()++;
        _mgr.num_edge() += cut->size;
    }

protected:
    void reference_best_cuts() override {
        _mgr.reset_is_lut_root();
        ForEachGraphPoV(_mgr) {
            uint po_fanin = _mgr.get_po(idx)[0].id();
            add_root_reference(po_fanin);
        }

        ForEachGraphLogicNodeRev(_mgr) {
            if (_mgr.num_est_ref(idx) == 0 || _mgr.is_lut_root(idx)) {
                continue;
            }
            reference_selected_cut(idx);
        }
    }

public:
    BackwardFlowAgd(mapper &mgr) : Backward(mgr) {}
};

// ========================================================================
// BackwardFlowPlain: reverse-topological re-selection with free-leaf zeroing
// for the plain (non-agdmap) area/flow path.
// Mirrors BackwardFlowAgd but uses plain cut_set(idx) only (no VID/wide-cut).
// ========================================================================
class BackwardFlowPlain : public Backward {
    CutCost compute_flow_cost(const Cut *root_cut) {
        return _mgr.compute_cut_cost(CutCostAlgo::FLOW, const_cast<Cut *>(root_cut));
    }

    // Select the best cut from plain cut_set, recomputing flow cost with current
    // (already-freed) leaf areas.  Excludes the trivial cut (last entry).
    const Cut *select_cut(uint idx, CutCost &best_cost) {
        auto &cuts = _mgr.cut_set(idx);
        Assert(cuts.size() >= 1);

        // If only trivial cut exists, use it
        const uint limit = cuts.size() > 1 ? cuts.size() - 1 : cuts.size();
        uint best_idx = 0;
        best_cost = compute_flow_cost(cuts[0]);
        for (uint i = 1; i < limit; ++i) {
            CutCost cost = compute_flow_cost(cuts[i]);
            if (_mgr.compare_with_required(idx, cost, best_cost) == CutCost::cmp_res::LWIN) {
                best_cost = cost;
                best_idx  = i;
            }
        }

        _mgr.set_best_cut(idx, cuts[best_idx]);
        return _mgr.best_cut(idx);
    }

    // Reference a leaf node: on first reference, zero its area/edge so upstream
    // cuts see shared logic as free (the free-leaf effect from agdmap cutSel).
    void add_root_reference(uint idx) {
        if (!_mgr.is_logic(idx)) {
            return;
        }
        if (_mgr.num_est_ref(idx)++ != 0) {
            return;
        }
        // First reference: free this leaf for upstream cost accounting.
        _mgr.area(idx) = 0;
        _mgr.edge(idx) = 0;
    }

    // Finalize idx as a LUT root: select its best cut (with freed-leaf costs),
    // update area/edge, mark as root, and reference all cut leaves.
    void reference_selected_cut(uint idx) {
        CutCost best_cost;
        const Cut *cut = select_cut(idx, best_cost);

        _mgr.set_is_lut_root(idx, true);
        float ratio = 1.0f / std::max(1.0f, _mgr.num_est_ref(idx));
        _mgr.area(idx) = best_cost.area * ratio;
        _mgr.edge(idx) = best_cost.edge * ratio;
        _mgr.arrival(idx) = best_cost.arr;

        ForEachCutLeaf(cut) {
            add_root_reference(leaf);
        }

        _mgr.num_area()++;
        _mgr.num_edge() += cut->size;
    }

protected:
    void reference_best_cuts() override {
        _mgr.reset_is_lut_root();

        // Seed: reference all PO fanins (mark first-time-seen as free).
        ForEachGraphPoV(_mgr) {
            uint po_fanin = _mgr.get_po(idx)[0].id();
            add_root_reference(po_fanin);
        }

        // Reverse topological sweep: for each referenced-but-not-yet-root node,
        // pick the best cut with current (freed) leaf costs.
        ForEachGraphLogicNodeRev(_mgr) {
            if (_mgr.num_est_ref(idx) == 0 || _mgr.is_lut_root(idx)) {
                continue;
            }
            reference_selected_cut(idx);
        }
    }

public:
    BackwardFlowPlain(mapper &mgr) : Backward(mgr) {}
};

// ========================================================================
// MappingPass
// ========================================================================
class MappingPass {
    mapper &_mgr;
    std::unique_ptr<Forward>  _forward;
    std::unique_ptr<Backward> _backward;

public:
    MappingPass(CutCostAlgo algo, mapper &mgr, int pass) : _mgr(mgr) {
        TIME_START(T)
        if (mgr.run_agdmap() && algo == CutCostAlgo::EXACT) {
            std::println(std::cerr, "AGDMap does not support EXACT mapping rounds.");
            std::abort();
        }
        if (algo == CutCostAlgo::FLOW)
            _forward = std::make_unique<ForwardFlow> (mgr);
        else if (algo == CutCostAlgo::EXACT)
            _forward = std::make_unique<ForwardExact> (mgr);
        else
            assert(0);
        const bool use_backward_flow_agd   = mgr.run_agdmap() && !mgr.config().first_pass;
        const bool use_backward_flow_plain = !mgr.run_agdmap() && !mgr.config().first_pass
                                             && mgr.config().area_mode();
        const bool use_backward_exact = algo == CutCostAlgo::EXACT && !mgr.config().first_pass;
        if (use_backward_flow_agd)
            _backward = std::make_unique<BackwardFlowAgd>(mgr);
        else if (use_backward_flow_plain)
            _backward = std::make_unique<BackwardFlowPlain>(mgr);
        else if (use_backward_exact)
            _backward = std::make_unique<BackwardExact>(mgr);
        else
            _backward = std::make_unique<Backward>(mgr);

        if (mgr.config().first_pass) {

        } else {
            // update the estimated reference number
            // do nothing, using previous pass's reference count for next cost compute
        }

        if (mgr.config().first_pass) {
            ///////////////////////////////////////
            _forward->impl();
            ///////////////////////////////////////
        } else if (!use_backward_exact) {
            _forward->reprioritize(algo);
        }


        if (_mgr.run_agdmap()) {
            _mgr.setup_agdmap_containers();
        }

        ///////////////////////////////////////
        _backward->impl();
        ///////////////////////////////////////
        mgr.num_delay() = mgr.calculate_delay();

        TIME_STOP(T)
        if (mgr.config().verbose) {
            std::println(std::cout, INFO1, pass, mgr.num_area(), mgr.num_edge(), Timer::formatted_time(cpu_T, 5),
                _mgr.num_merged(), _mgr.num_k_feasible(), _mgr.num_stored());
            if (mgr.config().delay_mode())
                std::println(std::cout, "    Delay {}", mgr.num_delay());
        }

        mgr.config().first_pass = false;

        if (_mgr.run_agdmap()) {
            return;
        }

    }

    ~MappingPass() {
        _mgr.num_area()       = 0;
        _mgr.num_edge()       = 0;
        _mgr.num_merged()     = 0;
        _mgr.num_k_feasible() = 0;
        _mgr.num_stored()     = 0;
    }
};

}
