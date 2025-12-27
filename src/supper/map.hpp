#pragma once

#include <algorithm>
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
#include <unordered_map>
#include <utility>
#include <vector>
#include <cassert>
#include <functional>
#include <deque>
#include <chrono>
#include <print>
#include <atomic>

#include "basic.hpp"

#include "cut.hpp"
#include "agdmap.hpp"

namespace abc {
    typedef struct Abc_Ntk_t_ Abc_Ntk_t;
}

namespace fox::supper {
// ========================================================================
// graph_t
// ========================================================================
class graph_t {
public:
    enum class node_type_t : uint8_t {
        ONE   ,
        PI    ,
        PO    ,
        LOGIC ,
        NONE
    };

    class node_t
    {
        uint        _size : 28;
        node_type_t _type :  4;
        Lit         _fanins[2];
    public:
        node_t()                                 : _size(0), _type(node_type_t::NONE), _fanins{} {}
        node_t(node_type_t type)                 : _size(0), _type(type), _fanins{}              {}
        node_t(node_type_t type, Lit f0)         : _size(1), _type(type), _fanins{f0, Lit(0)}    {}
        node_t(node_type_t type, Lit f0, Lit f1) : _size(2), _type(type), _fanins{f0, f1}        {}
       ~node_t() = default;

        Inline uint        size()     const { return _size;      }
        Inline node_type_t type()     const { return _type;      }
        Inline Lit operator[](uint i) const { return _fanins[i]; }

        Inline bool null    () const { return _type == node_type_t::NONE;  }
        Inline bool is_logic() const { return _type == node_type_t::LOGIC; }
        Inline bool is_pi   () const { return _type == node_type_t::PI;    }
        Inline bool is_po   () const { return _type == node_type_t::PO;    }

       friend class graph_t;
    };

protected:
    std::vector<node_t> _nodes;
    std::vector<uint>   _pi;
    std::vector<uint>   _po;

public:
    graph_t(uint max_node_num, uint num_pi = 0, uint num_po = 0) {
        _nodes.reserve(max_node_num);
        _pi   .reserve(num_pi);
        _po   .reserve(num_po);
    }

    ~graph_t() = default;

    Inline uint pwr()       const { return 0;             }
    Inline uint num_nodes() const { return _nodes.size(); }
    Inline uint num_po()    const { return _po.size();    }
    Inline uint num_pi()    const { return _pi.size();    }
    Inline uint num_logic() const { return num_nodes() - num_po() - num_pi() - 1; }

    Inline int begin()        const { return 1;                  }
    Inline int end()          const { return _nodes.size();      }
    Inline int rbegin()       const { return _nodes.size() - 1;  }
    Inline int rend()         const { return -1;                 }

    Inline int logic_begin()  const { return 1 + num_po() + num_pi(); }
    Inline int logic_end()    const { return end();                   }
    Inline int logic_rbegin() const { return _nodes.size() - 1;       }
    Inline int logic_rend()   const { return num_po() + num_pi();     }

    Inline int pi_begin()     const { return 1; }
    Inline int pi_end()       const { return 1 + num_pi(); }

    Inline const node_t &operator[](uint i) const { return _nodes[i];        }
    Inline const node_t &operator[](Lit  i) const { return _nodes[i.id()];   }
    Inline const node_t &get_pi(uint idx)   const { return _nodes[_pi[idx]]; }
    Inline const node_t &get_po(uint idx)   const { return _nodes[_po[idx]]; }

    Inline uint po_id(uint idx) const { return _po[idx]; }
    Inline uint pi_id(uint idx) const { return _pi[idx]; }

    void report(std::ostream &os);

    bool is_topologically_sorted() const;

    void *to_abc_ntk();

    /**
     * @brief Convert the graph structure to DOT format
     * 
     * @param path The path where the DOT file will be written
     * @return true if the file was successfully written, false otherwise
     */
    bool to_dot(const std::string &path) const;

    /**
     * @brief Convert the graph structure to Verilog netlist
     * 
     * @param path The path where the Verilog file will be written
     * @return true if the file was successfully written, false otherwise
     */
    bool write_to_verilog(const std::string &path) const;
};

#define ForEachGraphNode(mgr)                                              \
    for (int idx = (mgr).begin(); idx != (mgr).end(); ++idx)               \
        if ((mgr)[idx].null()) [[unlikely]] {} else

#define ForEachGraphLogicNode(mgr)                                         \
    for (int idx = (mgr).logic_begin(); idx != (mgr).logic_end(); ++idx)   \
        if (!(mgr)[idx].is_logic()) [[unlikely]] {} else

#define ForEachGraphNodeRev(mgr)                                           \
    for (int idx = (mgr).rbegin(); idx != (mgr).rend(); --idx)             \
        if ((mgr)[idx].null()) [[unlikely]] {} else

#define ForEachGraphLogicNodeRev(mgr)                                      \
    for (int idx = (mgr).logic_rbegin(); idx != (mgr).logic_rend(); --idx) \
        if (!(mgr)[idx].is_logic()) [[unlikely]] {} else

#define ForEachGraphPi(mgr) for (int idx = 0; idx != (mgr).num_pi(); ++idx)

#define ForEachGraphPo(mgr) for (int idx = 0; idx != (mgr).num_po(); ++idx)

#define ForEachGraphPoV(mgr)                                        \
    for (int idx = 0; idx != (mgr).num_po(); ++idx)                 \
        if (auto &n = (mgr).get_po(idx); n.size() && (mgr)[n[0]].is_logic())

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
    uint         max_cut_num {8};
    bool         verbose     {true };
    // internal
    bool         first_pass  {false};
    bool         enum_truth  {true};
    float        epsilon     {0.005};

    bool area_mode()  const { return opt_target == target_t::AREA;  }
    bool delay_mode() const { return opt_target == target_t::DELAY; }
    // bool edge_mode()  const { return opt_target == target_t::EDGE;  }
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

    static rank_fn GetRankFn(Config::target_t mode) {
        auto rank_fn_area_edge = [](const CutCost &lhs, const CutCost &rhs, float epsilon) -> auto {
            if (lhs.area + epsilon < rhs.area)  return cmp_res::LWIN;
            if (lhs.area - epsilon > rhs.area)  return cmp_res::RWIN;
            if (lhs.edge + epsilon < rhs.edge)  return cmp_res::LWIN;
            if (lhs.edge - epsilon > rhs.edge)  return cmp_res::RWIN;
            if (lhs.size < rhs.size)    return cmp_res::LWIN;
            if (lhs.size > rhs.size)    return cmp_res::RWIN;
            return cmp_res::SAME;
        };
        auto rank_fn_delay_size_area_edge = [](const CutCost &lhs, const CutCost &rhs, float epsilon) -> auto {
            if (lhs.arr < rhs.arr)    return cmp_res::LWIN;
            if (lhs.arr > rhs.arr)    return cmp_res::RWIN;
            if (lhs.size < rhs.size)  return cmp_res::LWIN;
            if (lhs.size > rhs.size)  return cmp_res::RWIN;
            if (lhs.area + epsilon < rhs.area)  return cmp_res::LWIN;
            if (lhs.area - epsilon > rhs.area)  return cmp_res::RWIN;
            if (lhs.edge + epsilon < rhs.edge)  return cmp_res::LWIN;
            if (lhs.edge - epsilon > rhs.edge)  return cmp_res::RWIN;
            return cmp_res::SAME;
        };
        if (mode == Config::target_t::AREA) // area
            return rank_fn_area_edge;
        else
            return rank_fn_delay_size_area_edge; // delay
    }
};

enum class CutCostAlgo {
    PRAETOR,
    FLOW,
    EXACT
};

enum class heuristic_t {
    PRAETOR,
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

    CutCost::rank_fn            _rank_fn ;
    std::vector<std::string>    _pi_names;
    std::vector<std::string>    _po_names;

    // -- Agdmap related
    Array<Gate *>               _gates;         // Simple gates
    Array<float>                _est_ref_agd;   // Estimated reference count for Agdmap
    std::atomic<uint>           _id_counter;    // Id counter for Agdmap virtual tree nodes

    // TODO: merge them into one struct
    std::unordered_map<uint, Cut *> _virual_cuts; // Virtual cuts for Agdmap
    std::unordered_map<uint, Area > _virual_area; // Virtual area for Agdmap
    std::unordered_map<uint, Edge > _virual_edge; // Virtual edge for Agdmap

    mutable Timer _timer;

    uint _bc_size  {0};
    uint _num_area {0};
    uint _num_edge {0};
    uint _num_delay{0};

    uint64_t _stat_cut[3]{0};

public:
    friend class enumerate_cut;

    using CutSet = std::vector<Cut *>;

    mapper(uint max_node_num, uint num_pi = 0, uint num_po = 0);
    ~mapper();

    const Config &config() const { return _cfg; }

    void initialize();

    bool run_agdmap() const { return _cfg.map_impl == Config::AGDMAP; }

    uint num_virtual_nodes() const {
        return _id_counter.load() - VID;
    }

    // reset flags
    Inline void reset_est_ref () { std::fill(_est_ref .begin(), _est_ref .end(), 0       ); }
    Inline void reset_required() { std::fill(_required.begin(), _required.end(), kMaxTime); }

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
        return n >= VID ? _virual_area[n - VID] : _area[n];
    }

    template<Indexable T>
    Inline Edge &edge(T n) {
        Assert(is_logic(n) || is_pi(n));
        return n >= VID ? _virual_edge[n - VID] : _edge[n];
    }

    template<Indexable T> Inline Time &arrival(T n) { return _arrival[n]; }
    template<Indexable T> Inline Time &required(T n) { return _required[n]; }
    template<Indexable T> Inline float &num_est_ref(T n) { return n >= VID ? _est_ref_agd[n] : _est_ref[n];  }
    template<Indexable T> Inline uint  &num_ref    (T n) { return _int_ref[n];  }

    template<Indexable T>
    Inline const Cut *best_cut(T n) {
        Assert(is_logic(n));
        return n >= VID ? _virual_cuts[n - VID] : _best_cuts[n];
    }

    template<Indexable T>
    void set_best_cut(T n, const Cut *cut) {
        ForEachCutLeaf(cut) {
            Assert(is_pi(leaf) || is_logic(leaf));
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

    // mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm
    // Agdmap related functions
    void create_simple_gates(uint max_size);

    Inline Gate *gate(uint id) {
        return _gates[id];
    }

    Inline uint fetch_free_id(uint num) {
        return _id_counter.fetch_add(num, std::memory_order_relaxed);
    }

    Inline void setup_agdmap_containers() {
        _est_ref_agd.clear();
        _est_ref_agd.resize(_id_counter.load() - VID, 0);
    }

    // TODO: using parallel hash map.
    Inline void register_virtual_cut(uint id, Cut *cut) {
        if constexpr (kDebugBuild) {
            ForEachCutLeaf(cut) {
                Assert(is_pi(leaf) || is_logic(leaf));
            }
        }
        Assert(id >= VID && id < AGD_MAX_ID);
        Assert(cut->size <= AGD_MAX_LUT_SIZE);
        Assert(_virual_cuts.find(id) == _virual_cuts.end() || _virual_cuts.find(id)->second == cut);
        _virual_cuts[id - VID] = cut;
    }

    Inline void register_virtual_area(uint id, Area area) {
        Assert(id >= VID && id < AGD_MAX_ID);
        Assert(_virual_area.find(id) == _virual_area.end() || _virual_area.find(id)->second == area);
        _virual_area[id - VID] = area;
    }

    Inline void register_virtual_edge(uint id, Edge edge) {
        Assert(id >= VID && id < AGD_MAX_ID);
        Assert(_virual_edge.find(id) == _virual_edge.end() || _virual_edge.find(id)->second == edge);
        _virual_edge[id - VID] = edge;
    }

    Timer &timer() { return _timer; }

    graph_t *run_lut_mapping(const Config &cfg);

    graph_t *create_mapped_graph();

    void *create_abc_ntk_from_mapping(bool use_truth_table = true, bool use_cut_truth = true);

    Time calculate_delay();

    Area ref_mffc(const Cut *cut);
    Edge rip_mffc(const Cut *cut);

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
        ForEachGraphPoV(_mgr) {
            uint po_fanin = _mgr.get_po(idx)[0].id();
            if (_mgr.num_est_ref(po_fanin)++ == 0 && _mgr.is_logic(po_fanin)) {
                reference_cut_rec(po_fanin);
            }
        }
    }

    void propagate_required() {}

public:
    Backward(mapper &mgr) : _mgr(mgr) {}
    virtual ~Backward() = default;

    virtual void impl() {
        _mgr.timer().start("backward");
        _mgr.reset_est_ref();

        if (_mgr.config().delay_mode()) {
            _mgr.reset_required();
        }

        reference_best_cuts();

        if (_mgr.config().delay_mode()) {
            propagate_required();
        }
        _mgr.timer().stop("backward");
    }
};

// ========================================================================
// MappingPass
// ========================================================================
class MappingPass {
    mapper &_mgr;
    std::unique_ptr<Forward>  _forward;
    std::unique_ptr<Backward> _backward;

    void improve_mapping_exactly(mapper &mgr);

public:
    MappingPass(CutCostAlgo algo, mapper &mgr, int pass) : _mgr(mgr) {
        TIME_START(T)
        if (algo == CutCostAlgo::FLOW)
            _forward = std::make_unique<ForwardFlow> (mgr);
        else if (algo == CutCostAlgo::EXACT)
            _forward = std::make_unique<ForwardExact> (mgr);
        else
            assert(0);
        _backward = std::make_unique<Backward>(mgr);

        if (!mgr.config().first_pass) {
            // update the estimated reference number
            // do nothing, using previous pass's reference count for next cost compute
        }

        ///////////////////////////////////////
        _forward->impl();
        ///////////////////////////////////////

        if (_mgr.run_agdmap()) {
            _mgr.setup_agdmap_containers();
        }

        ///////////////////////////////////////
        _backward->impl();
        ///////////////////////////////////////

        TIME_STOP(T)
        if (mgr.config().verbose) {
            std::println(std::cout, INFO1, pass, mgr.num_area(), mgr.num_edge(), Timer::formatted_time(cpu_T, 5),
                _mgr.num_merged(), _mgr.num_k_feasible(), _mgr.num_stored());
        }

        if (_mgr.run_agdmap()) {
            return;
        }

        ///////////////////////////////////////
        improve_mapping_exactly(mgr);
        ///////////////////////////////////////
    }

    ~MappingPass() {
        if (!_mgr.run_agdmap()) { // if agdmap is running, some non-best cuts are referenced by fanouts' best cuts.
            _mgr.free_cuts();
        }
        _mgr.num_area()       = 0;
        _mgr.num_edge()       = 0;
        _mgr.num_merged()     = 0;
        _mgr.num_k_feasible() = 0;
        _mgr.num_stored()     = 0;
    }
};

}
