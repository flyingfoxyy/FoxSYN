// #pragma once

// #include <sys/types.h>
// #include <vector>
// #include <cstdint>
// #include <cassert>

// namespace praetor {

// // template <typename T>
// // class Cut {
// //     uint size;
// //     uint sign;
// //     uint leaves[0];
// // };

// template <typename N>
// concept Ntk = requires(N ntk, uint id, uint idx) {
//     // 要求有类型别名 node_t
//     typename N::node_t;

//     // 要求有 num_nodes()，返回值可转换为 size_t
//     { ntk.num_nodes() } -> std::convertible_to<uint>;
//     { ntk.num_pi() }    -> std::convertible_to<uint>;
//     { ntk.num_po() }    -> std::convertible_to<uint>;

//     { ntk.get_node(id) } -> std::convertible_to<typename N::node_t>;
//     { ntk.is_pi(id) }    -> std::convertible_to<bool>;
//     { ntk.is_po(id) }    -> std::convertible_to<bool>;
//     { ntk.is_logic(id) } -> std::convertible_to<bool>;
//     { ntk.null(id) }     -> std::convertible_to<bool>;

//     { ntk.fanin0(id, idx) }  -> std::convertible_to<uint>;
//     { ntk.fanin1(id, idx) }  -> std::convertible_to<uint>;
//     { ntk.fanin0_inv(id) }   -> std::convertible_to<bool>;

// };

// template<typename Ntk>
// class Praetor : public Ntk {
// };

// }
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <ostream>
#include <utility>
#include <vector>
#include <cassert>
#include <functional>
#include <deque>

#include "basic.hpp"

namespace abc {
    typedef struct Abc_Ntk_t_ Abc_Ntk_t;
}

namespace fox::supper {

#define SIGNATURE(x, BITNUM) (1 << ((x) % ((BITNUM) - 1)))

class Cut {
public:
    static constexpr uint BIT_NUM_SIZE = 7;
    static constexpr uint BIT_NUM_SIGN = std::numeric_limits<uint>::digits - BIT_NUM_SIZE;
    static constexpr uint MAX_CUT_SIZE = (1 << BIT_NUM_SIZE) - 1;
    static constexpr uint SIGN_MASK    = BIT_NUM_SIGN - 1;
    static constexpr uint R            = 0;

// protected:
    uint size : BIT_NUM_SIZE;
    uint sign : BIT_NUM_SIGN;
    uint leaves[R];

// public:
    Cut(uint *begin, uint *end, Sign s) : size(end - begin), sign(s) {
        std::memcpy(leaves, begin, size);
    }

    Cut(const Cut &cut) : size(cut.size), sign(cut.sign) {
        std::memcpy(leaves, cut.leaves, sizeof(uint) * size);
    }

    Cut(uint id) : size(1), sign(SIGNATURE(id, BIT_NUM_SIGN)) { leaves[0] = id; }

    // uint size() const { return _size; }
    // uint sign() const { return _sign; }

    // uint operator[](uint i) const { assert(i < _size); return leaves[i]; }

    Cut *next() const {
        if constexpr (R != 0) {
            const int extra_size = R >= size ? 0 : (size - R);
            assert(extra_size >= 0);
            return reinterpret_cast<Cut *>(
                reinterpret_cast<uintptr_t>(this) + sizeof(Cut) + sizeof(uint) * extra_size
            );
        } else {
            return reinterpret_cast<Cut *>(
                reinterpret_cast<uintptr_t>(this) + sizeof(Cut) + sizeof(uint) * size
            );
        }
    }

    Cut *clone() const {
        return allocate<Cut>(size, *this);
    }

    std::string to_str() const {
        std::string res = "{ ";
        for (int i = 0; i != size; ++i) {
            res += std::to_string(leaves[i]) + " ";
        }
        res += "}";
        return res;
    }
};

class function_db {
    std::vector<word> _truth_tables;
public:
    function_db() = default;
    ~function_db() = default;

    word get_truth_table(uint id) {
        return _truth_tables[id];
    }
};


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

        uint        size()     const { return _size;      }
        node_type_t type()     const { return _type;      }
        Lit operator[](uint i) const { return _fanins[i]; }

        bool null    () const { return _type == node_type_t::NONE;  }
        bool is_logic() const { return _type == node_type_t::LOGIC; }
        bool is_pi   () const { return _type == node_type_t::PI;    }
        bool is_po   () const { return _type == node_type_t::PO;    }

       friend class graph_t;
    };

protected:
    std::vector<node_t> _nodes;
    std::vector<uint>   _pi;
    std::vector<uint>   _po;
    function_db        *_db = nullptr;

public:
    graph_t(uint max_node_num, uint num_pi = 0, uint num_po = 0) {
        _nodes.reserve(max_node_num);
        _pi   .reserve(num_pi);
        _po   .reserve(num_po);
    }

    ~graph_t() = default;

    void add_node(node_type_t type, Lit f0 = Lit{}, Lit f1 = Lit{}) {
        _nodes.emplace_back(type, f0, f1);
        if      (type == node_type_t::PI)   _pi.push_back(_nodes.size() - 1);
        else if (type == node_type_t::PO)   _po.push_back(_nodes.size() - 1);
    }

    uint num_nodes() const { return _nodes.size(); }
    uint num_po()    const { return _po.size();    }
    uint num_pi()    const { return _pi.size();    }
    uint num_logic() const { return num_nodes() - num_po() - num_pi(); }

    int begin()  const { return 0;                  }
    int end()    const { return _nodes.size();      }
    int rbegin() const { return _nodes.size() - 1;  }
    int rend()   const { return -1;                 }

    const node_t &operator[](uint i) const { return _nodes[i];        }
    const node_t &pi(uint idx)       const { return _nodes[_pi[idx]]; }
    const node_t &po(uint idx)       const { return _nodes[_po[idx]]; }

    void report(std::ostream &os) {
        os << "graph stats: ";
        os << "PI "    << num_pi()    << "\t";
        os << "PO "    << num_po()    << "\t";
        os << "LOGIC " << num_logic() << "\t";
        os << "\n";
    }

    abc::Abc_Ntk_t *to_ntk();
};

#define ForEachGraphNode(mgr)                                  \
    for (int idx = (mgr).begin(); idx != (mgr).end(); ++idx)   \
        if ((mgr)[idx].null()) [[unlikely]] {} else

#define ForEachGraphLogicNode(mgr)                             \
    for (int idx = (mgr).begin(); idx != (mgr).end(); ++idx)   \
        if (!(mgr)[idx].is_logic()) [[unlikely]] {} else

#define ForEachGraphNodeRev(mgr)                               \
    for (int idx = (mgr).rbegin(); idx != (mgr).rend(); --idx) \
        if ((mgr)[idx].null()) [[unlikely]] {} else

#define ForEachGraphLogicNodeRev(mgr)                          \
    for (int idx = (mgr).rbegin(); idx != (mgr).rend(); --idx) \
        if (!(mgr)[idx].is_logic()) [[unlikely]] {} else

#define ForEachGraphPi(mgr) for (int idx = 0; idx != (mgr).num_pi(); ++idx)
#define ForEachGraphPo(mgr) for (int idx = 0; idx != (mgr).num_po(); ++idx)

// class enumerate_cut;

class Config {
public:
    enum class target_t : uint8_t {
        AREA,
        DELAY,
        EDGE
        // EDGEArea
    };

    enum class map_impl_t : uint32_t {
        PRIORITY_CUTS = 0x1,
        PRAETOR       = 0x2,
        AGDMAP        = 0x4,
        ACDMAP        = 0x8
    };

    // user controllable
    target_t     opt_target  {target_t::DELAY};
    uint         map_impl    {(uint)map_impl_t::PRIORITY_CUTS};
    uint         cut_size    {6};
    uint         lut_size    {6};
    uint         max_cut_num {8};
    bool         verbose     {true };
    // internal
    bool         first_pass  {false};
    float        epsilon     {0.005};

    bool area_mode()  const { return opt_target == target_t::AREA;  }
    bool delay_mode() const { return opt_target == target_t::DELAY; }
    // bool edge_mode()  const { return opt_target == target_t::EDGE;  }
};

class mapper : public graph_t {
    Config        _cfg{};

    SigMap<uint>  _int_ref;
    SigMap<float> _est_ref;
    SigMap<Area>  _area;
    SigMap<Edge>  _edge;
    SigMap<Time>  _arrival;
    SigMap<Time>  _required;
    SigMap<Cut *> _best_cut;

    SigMap<std::vector<Cut *>> _cuts; // TODO: Using a pointer
    
public:
    friend class enumerate_cut;

    mapper(uint max_node_num, uint num_pi = 0, uint num_po = 0)
    : graph_t(max_node_num, num_pi, num_po)
    {
        constexpr uint kMax = std::numeric_limits<uint>::max() / 2;
        if (max_node_num > kMax) [[unlikely]] {
            std::cout << "Node number exceeds maximum limit " << kMax << ", quit.\n";
            std::exit(1);
        }
        _int_ref .resize(max_node_num, 0);
        _est_ref .resize(max_node_num, 0);
        _area    .resize(max_node_num, 0);
        _edge    .resize(max_node_num, 0);
        _arrival .resize(max_node_num, 0);
        _required.resize(max_node_num, kMaxTime);
        _cuts    .resize(max_node_num, {});
        _best_cut.resize(max_node_num, nullptr);
    }
    ~mapper() = default;

    const Config &config() const { return _cfg; }

    void initialize();

    // reset flags
    void reset_est_ref () { std::fill(_est_ref .begin(), _est_ref .end(), 0       ); }
    void reset_required() { std::fill(_required.begin(), _required.end(), kMaxTime); }

    // creators
    static mapper *create_from_aig(void *ntk);

    template<Indexable IDX> std::vector<Cut *> &cut_set(IDX n) { return _cuts[n]; }

    template<Indexable IDX> Area  area(IDX n)     const { return _area[n];     }
    template<Indexable IDX> Area &area(IDX n)           { return _area[n];     }
    template<Indexable IDX> Edge  edge(IDX n)     const { return _edge[n];     }
    template<Indexable IDX> Edge &edge(IDX n)           { return _edge[n];     }
    template<Indexable IDX> Time  arrival(IDX n)  const { return _arrival[n];  }
    template<Indexable IDX> Time &arrival(IDX n)        { return _arrival[n];  }
    template<Indexable IDX> Time  required(IDX n) const { return _required[n]; }
    template<Indexable IDX> Time &required(IDX n)       { return _required[n]; }

    template<Indexable IDX> float  num_est_ref(IDX n) const { return _est_ref[n]; }
    template<Indexable IDX> float &num_est_ref(IDX n)       { return _est_ref[n]; }
    template<Indexable IDX> uint   num_ref(IDX n)     const { return _int_ref[n]; }
    template<Indexable IDX> uint  &num_ref(IDX n)           { return _int_ref[n]; }

    template<Indexable IDX> Cut *&best_cut(IDX n) { return _best_cut[n]; }

    graph_t *run_lut_mapping(const Config &cfg);

    graph_t *create_mapped_graph();
};


struct CutCost {
    enum class cmp_res {
        LWIN = 0,
        RWIN = 1,
        SAME = 2
    };
    using rank_fn = std::function<cmp_res(const CutCost &, const CutCost &, float)>;

    Area area {0};
    Edge edge {0};
    Time arr  {123456};
    uint size {0};
    uint idx  {0};

    static rank_fn GetRankFn(int mode) {
        auto rank_fn_area_edge = [](const CutCost &lhs, const CutCost &rhs, float epsilon) -> auto {
            if (lhs.area + epsilon < rhs.area)  return cmp_res::LWIN;
            if (lhs.area - epsilon > rhs.area)  return cmp_res::RWIN;
            if (lhs.edge + epsilon < rhs.edge)  return cmp_res::LWIN;
            if (lhs.edge - epsilon > rhs.edge)  return cmp_res::RWIN;
            if (lhs.size < rhs.size)    return cmp_res::LWIN;
            if (lhs.size > rhs.size)    return cmp_res::RWIN;
            return cmp_res::SAME;
        };
        if (mode == 0) // area
            return rank_fn_area_edge;
        return rank_fn_area_edge;
    }

};

/*
template <typename T>
class CutList {
    std::deque<CutCost>  _cost;
    CutCost::rank_fn     _fn;
    uint                 _cap;
public:
    CutList(uint max_num) : _cap(max_num) {
        // _cost.reserve(max_num);
    }

    void add(CutCost cost) {
        if (_cost.size() == _cap) {
            // compare with the last one first
            if (_fn(cost, _cost.back()) != CutCost::cmp_res::LWIN) {
                // same or worse than the last one, discard at once
                // deallocate(cut);
                return;
            } else {
                // cut could be saved
                // check if cut can be the best one
                auto res = _fn(cost, _cost.front());
                if (res == CutCost::cmp_res::SAME) [[unlikely]] {
                    return;
                } else
                if (res == CutCost::cmp_res::LWIN) { // better than the current best one
                    _cost.push_front(cost);
                    _cost.pop_back();
                    return;
                }
                // cost is better than last one, worse than best one 



            }
        } else {
            _cost.push_back(cost);
            if (_cost.size() == _cap) {
                std::sort(_cost.begin(), _cost.end(), _fn);
            }
        }
        assert(_cost.size() <= _cap);
    }

    // std::vector<Cut *> &get() { return _list; }
};
*/

enum class CutCostAlgo {
    PRAETOR,
    FLOW,
    EXACT
};


// normal enumerate cut way
template <CutCostAlgo algo>
class CutEnumerator {
public:
    static void run(mapper &mgr, uint id) {
        const auto &node = mgr[id];
        if (!node.is_logic()) [[unlikely]]
            return;

        const Config &cfg = mgr.config();

        Lit f0 = node[0];
        Lit f1 = node[1];

        const std::vector<Cut *> &cuts0 = mgr.cut_set(f0);
        const std::vector<Cut *> &cuts1 = mgr.cut_set(f1);

        const uint merge_num_upper = (cuts0.size() + 1) * (cuts1.size() + 1);
        // If num_c0 * num_c1 > max_cut_num, we need to prune the cuts, i.e., full cut ranking is needed
        // However, if num_c0 * num_c1 <= max_cut_num, we only need to merge the cuts and find the best one
        // Complexity reduced from O(nlogn) to O(n)
        const bool sort = merge_num_upper > cfg.max_cut_num;

        // int merge_idx = -1;
        // int num_valid = 0;

        // static_assert(Cut::R); Cut t0(f0.id()); Cut t1(f1.id());
        Cut *t0 = allocate<Cut>(1, f0.id());
        Cut *t1 = allocate<Cut>(1, f1.id());

        std::vector<Cut *>   gen_cuts;
        std::vector<CutCost> gen_costs;
    
        gen_cuts .reserve(merge_num_upper / 1.5);
        gen_costs.reserve(merge_num_upper / 1.5);

        uint buffer[Cut::MAX_CUT_SIZE << 1] {0};

        for (int i0 = -1; i0 != cuts0.size(); ++i0) { Cut *c0 = i0 < 0 ? t0 : cuts0[i0];
        for (int i1 = -1; i1 != cuts1.size(); ++i1) { Cut *c1 = i1 < 0 ? t1 : cuts1[i1];
            // ++merge_idx;
            if (c0->size + c1->size > cfg.cut_size && __builtin_popcount(c0->sign | c1->sign) > cfg.cut_size)
                continue;
            std::memset(buffer, 0, sizeof(uint) * (cfg.cut_size * 2));
            // TODO: using optimized merge
            uint *end = std::set_union(
                c0->leaves, c0->leaves + c0->size,
                c1->leaves, c1->leaves + c1->size,
                buffer
            );
            const int size = end - buffer;
            if (size > cfg.cut_size)
                continue;
            Cut *cut = allocate<Cut>(size, buffer, end);
            gen_cuts.push_back(cut);
            // compute cut cost according to the algo
            CutCost cost;
            if constexpr (algo == CutCostAlgo::FLOW) {
                cost.size = cut->size;
                cost.idx  = gen_cuts.size() - 1;
                // area-flow/edge-flow
                for (int i = 0; i != cut->size; ++i) {
                    uint leaf = cut->leaves[i];
                    cost.area += mgr.area(leaf);
                    cost.edge += mgr.edge(leaf);
                }
            } else if constexpr (algo == CutCostAlgo::EXACT) {
                assert(0);
            } else if constexpr (algo == CutCostAlgo::PRAETOR) {
                assert(0);
            } else {
                static_assert(0);
            }
            gen_costs.push_back(cost);
        }} // end merge cuts

        deallocate(t0);
        deallocate(t1);

        float epsilon = cfg.epsilon;
        CutCost::rank_fn fn = CutCost::GetRankFn(0);
        auto fn_wrap = [epsilon, fn](const CutCost &lhs, const CutCost &rhs) -> bool {
            return fn(lhs, rhs, epsilon) == CutCost::cmp_res::LWIN;
        };

        if (sort) {
            std::sort(gen_costs.begin(), gen_costs.end(), fn_wrap);
        } else {
            auto best_itr = std::min_element(gen_costs.begin(), gen_costs.end(), fn_wrap);
            assert(best_itr != gen_costs.end());
            std::iter_swap(gen_costs.begin(), best_itr);
        }

        // mapping cost ranking back to cuts
        std::vector<Cut *> &cuts = mgr.cut_set(id);
        for (int i = 0; i != gen_costs.size() && i != cfg.max_cut_num; ++i) {
            Cut *&cut = gen_cuts[gen_costs[i].idx];
            cuts.push_back(cut);
            cut = nullptr;
        }
        for (Cut *cut : gen_cuts) {
            deallocate(cut);
        }
        mgr.best_cut(id) = cuts.front()->clone();
        // Set the node area/edge/arr info
        mgr.area(id) = gen_costs.front().area / mgr.num_est_ref(id);
        mgr.edge(id) = gen_costs.front().edge / mgr.num_est_ref(id);
    }
};

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
        ForEachGraphNode(_mgr) {
            CutEnumerator<CutCostAlgo::FLOW>::run(_mgr, idx);
        }
    }
};

class ForwardExact : public Forward {
public:
    ForwardExact(mapper &mgr) : Forward(mgr) {}

    virtual void impl() {
        ForEachGraphNode(_mgr) {
            CutEnumerator<CutCostAlgo::EXACT>::run(_mgr, idx);
        }
    }
};

// generic backword pass
// visit nodes in reversed topological order and set the best cuts and propagate required times
class Backword {
protected:
    mapper &_mgr;

    uint _num_area;
    uint _num_lut;
    uint _num_edge;
    uint _num_delay;

    virtual void reference_best_cuts() {
        ForEachGraphPo(_mgr) {
            const auto &po = _mgr.po(idx);
            if (po.size()) [[likely]] {
                ++_mgr.num_est_ref(po[0]);
            }
        }

        ForEachGraphLogicNodeRev(_mgr) {
            if (_mgr.num_est_ref(idx) == 0)
                continue;
            Cut *cut = _mgr.best_cut(idx);
            assert(cut);
            for (int i = 0; i != cut->size; ++i) {
                uint leaf_id = cut->leaves[i];
                ++_mgr.num_est_ref(leaf_id);
            }
            _num_lut  += 1;
            _num_edge += cut->size;
        }
    }

    void propagate_required() {}

public:
    Backword(mapper &mgr) : _mgr(mgr), _num_area(0), _num_lut(0), _num_edge(0), _num_delay(0) {}
    virtual ~Backword() = default;

    virtual void impl() {
        _mgr.reset_est_ref();
        if (_mgr.config().delay_mode()) {
            _mgr.reset_required();
        }

        reference_best_cuts();

        if (_mgr.config().delay_mode()) {
            propagate_required();
        }
    }

    void report(std::ostream &os) {
        os << "mapping stats: ";
        os << "LUT "   << _num_lut  << "\t";
        os << "EDGE "  << _num_edge << "\t";
        os << "\n";
    }
};

class MappingPass {
    std::unique_ptr<Forward>  _forward;
    std::unique_ptr<Backword> _backword;

public:
    MappingPass(CutCostAlgo algo, mapper &mgr) {
        if (algo == CutCostAlgo::FLOW)
            _forward = std::make_unique<ForwardFlow> (mgr);
        else if (algo == CutCostAlgo::EXACT)
            _forward = std::make_unique<ForwardExact> (mgr);
        else
            assert(0);
        _backword = std::make_unique<Backword>(mgr);

        if (!mgr.config().first_pass) {
            // update the estimated reference number
            // do nothing, using previous pass's reference count for next cost compute
        }
        _forward ->impl();
        _backword->impl();

        if (mgr.config().verbose) {
            mgr.report(std::cout);
            _backword->report(std::cout);
        }
    }

    ~MappingPass() = default;
};

}
