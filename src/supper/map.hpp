#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>
#include <cassert>
#include <functional>
#include <deque>

#include "basic.hpp"

namespace fox::supper {

#define SIGNATURE(x, BITNUM) (1 << (x % (BITNUM - 1)))

class Cut {
public:
    static constexpr uint BIT_NUM_SIZE = 7;
    static constexpr uint BIT_NUM_SIGN = std::numeric_limits<uint>::digits - BIT_NUM_SIZE;
    static constexpr uint MAX_CUT_SIZE = (1 << BIT_NUM_SIZE) - 1;
    static constexpr uint SIGN_MASK    = BIT_NUM_SIGN - 1;
    static constexpr uint R            = 1;

    uint size : BIT_NUM_SIZE;
    uint sign : BIT_NUM_SIGN;
    uint leaves[R] {0};

    Cut(uint *begin, uint *end, Sign s) : size(end - begin), sign(s) {
        std::memcpy(leaves, begin, size);
    }

    Cut(const Cut &cut) : size(cut.size), sign(cut.sign) {
        std::memcpy(leaves, cut.leaves, sizeof(uint) * size);
    }

    Cut(uint id) : size(1), sign(SIGNATURE(id, BIT_NUM_SIGN)) { leaves[0] = id; }

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
        Lit         _fanins[2]{0};
    public:
        node_t()                                 : _size(0), _type(node_type_t::NONE)     {}
        node_t(node_type_t type)                 : _size(0), _type(type)                  {}
        node_t(node_type_t type, Lit f0)         : _size(1), _type(type), _fanins{f0, 0}  {}
        node_t(node_type_t type, Lit f0, Lit f1) : _size(2), _type(type), _fanins{f0, f1} {}
       ~node_t() = default;

        uint        size()  const { return _size; }
        node_type_t type()  const { return _type; }

        Lit         operator[](uint i) const { return _fanins[i]; }

        bool is_logic() const { return _type == node_type_t::LOGIC; }
        bool is_pi   () const { return _type == node_type_t::PI;    }
        bool is_po   () const { return _type == node_type_t::PO;    }

       friend class graph_t;
    };

protected:
    std::vector<node_t>   _nodes;
    std::vector<node_t *> _pi;
    std::vector<node_t *> _po;

public:
    graph_t(uint max_node_num, uint num_pi = 0, uint num_po = 0) {
        _nodes.reserve(max_node_num);
        if (num_pi)
            _pi.reserve(num_pi);
        if (num_po)
            _po.reserve(num_po);
    }

    graph_t(uint64_t num_pin) {

    }

    ~graph_t() = default;
    
    void add_node(node_type_t type, Lit f1 = 0, Lit f2 = 0) {
        node_t &node = _nodes.emplace_back(type, f1, f2);
        if (type == node_type_t::PI)
            _pi.push_back(&node);
        else if (type == node_type_t::PO)
            _po.push_back(&node);
    }

    uint num_nodes() const { return _nodes.size(); }
    uint num_po()    const { return _po.size();    }
    uint num_pi()    const { return _pi.size();    }

    int begin()  const { return 0;                  }
    int end()    const { return _nodes.size();      }
    int rbegin() const { return _nodes.size() - 1;  }
    int rend()   const { return -1;                 }

    const node_t &operator[](uint i) { return _nodes[i]; }
};

class enumerate_cut;

class mapping_config {
public:
    uint  cut_size;
    uint  lut_size;
    uint  prune_mode;
    uint  opt_target;
    uint  max_cut_num;
    float epsilon;
};

class mapper : public graph_t {
    std::vector<uint>  _int_ref;
    std::vector<float> _est_ref;
    std::vector<Area>  _area;  
    std::vector<Edge>  _edge;
    std::vector<Time>  _arrival;
    std::vector<Cut *> _best_cut;
    std::vector<std::vector<Cut *>> _cuts;
    
public:
    friend class enumerate_cut;

    mapper(uint max_node_num, uint num_pi = 0, uint num_po = 0)
    : graph_t(max_node_num, num_pi, num_po)
    {
        _int_ref .resize(max_node_num, 0);
        _est_ref .resize(max_node_num, 0);
        _area    .resize(max_node_num, 0);
        _edge    .resize(max_node_num, 0);
        _arrival .resize(max_node_num, 0);
        _cuts    .resize(max_node_num, {});
        _best_cut.resize(max_node_num, nullptr);
    }
    ~mapper() = default;

    std::vector<Cut *> &cut_set(Lit n) { return _cuts[n.id()]; }

    Area &area(Lit n)    { return _area[n.id()];    }
    Edge &edge(Lit n)    { return _edge[n.id()];    }
    Time &arrival(Lit n) { return _arrival[n.id()]; }
    Area &area(uint n)    { return _area[n];    }
    Edge &edge(uint n)    { return _edge[n];    }
    Time &arrival(uint n) { return _arrival[n]; }

    Area area(Lit n)    const { return _area[n.id()];    }
    Edge edge(Lit n)    const { return _edge[n.id()];    }
    Time arrival(Lit n) const { return _arrival[n.id()]; }
    Area area(uint n)    const { return _area[n];    }
    Edge edge(uint n)    const { return _edge[n];    }
    Time arrival(uint n) const { return _arrival[n]; }

    Cut *&best_cut(uint n) { return _best_cut[n]; }

    float num_est_ref(uint n) const { return _est_ref[n]; }

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
            if (lhs.area + epsilon < rhs.area)
                return cmp_res::LWIN;
            if (lhs.area - epsilon > rhs.area)
                return cmp_res::RWIN;
            if (lhs.edge + epsilon < rhs.edge)
                return cmp_res::LWIN;
            if (lhs.edge - epsilon > rhs.edge)
                return cmp_res::RWIN;
            if (lhs.size < rhs.size)
                return cmp_res::LWIN;
            if (lhs.size > rhs.size)
                return cmp_res::RWIN;
            return cmp_res::SAME;
        };
        if (mode == 0) // area
            return rank_fn_area_edge;
        return rank_fn_area_edge;
    }

};



static CutCost compute_cost(uint idx, mapper &mgr, Cut *cut, const mapping_config &cfg) {
    CutCost cost;
    cost.size = cut->size;
    cost.idx  = idx;
    // area-flow/edge-flow
    for (int i = 0; i != cut->size; ++i) {
        uint leaf = cut->leaves[i];
        cost.area += mgr.area(leaf);
        cost.edge += mgr.edge(leaf);
    }
    return cost;
}

// template <typename T>
// class CutList {
//     std::deque<CutCost>  _cost;
//     CutCost::rank_fn     _fn;
//     uint                 _cap;
// public:
//     CutList(uint max_num) : _cap(max_num) {
//         // _cost.reserve(max_num);
//     }

//     void add(CutCost cost) {
//         if (_cost.size() == _cap) {
//             // compare with the last one first
//             if (_fn(cost, _cost.back()) != CutCost::cmp_res::LWIN) {
//                 // same or worse than the last one, discard at once
//                 // deallocate(cut);
//                 return;
//             } else {
//                 // cut could be saved
//                 // check if cut can be the best one
//                 auto res = _fn(cost, _cost.front());
//                 if (res == CutCost::cmp_res::SAME) [[unlikely]] {
//                     return;
//                 } else
//                 if (res == CutCost::cmp_res::LWIN) { // better than the current best one
//                     _cost.push_front(cost);
//                     _cost.pop_back();
//                     return;
//                 }
//                 // cost is better than last one, worse than best one 



//             }
//         } else {
//             _cost.push_back(cost);
//             if (_cost.size() == _cap) {
//                 std::sort(_cost.begin(), _cost.end(), _fn);
//             }
//         }
//         assert(_cost.size() <= _cap);
//     }

//     // std::vector<Cut *> &get() { return _list; }
// };

// normal enumerate cut way
class enumerate_cut {
    mapper &_mgr;
public:
    enumerate_cut(mapper &mgr) : _mgr(mgr) {}
    ~enumerate_cut() = default;

    void run(uint id, const mapping_config &cfg) {
        const auto &node = _mgr[id];
        if (!node.is_logic()) [[unlikely]]
            return;

        Lit f0 = node[0];
        Lit f1 = node[1];

        const std::vector<Cut *> &cuts0 = _mgr.cut_set(f0);
        const std::vector<Cut *> &cuts1 = _mgr.cut_set(f1);

        const uint merge_num_upper = (cuts0.size() + 1) * (cuts1.size() + 1);
        // If num_c0 * num_c1 > max_cut_num, we need to prune the cuts, i.e., full cut ranking is needed
        // However, if num_c0 * num_c1 <= max_cut_num, we only need to merge the cuts and find the best one
        // Complexity reduced from O(nlogn) to O(n)
        const bool sort = merge_num_upper > cfg.max_cut_num;

        // int merge_idx = -1;
        // int num_valid = 0;

        static_assert(Cut::R); Cut t0(f0.id()); Cut t1(f1.id());

        std::vector<Cut *>   gen_cuts;
        std::vector<CutCost> gen_costs;
    
        gen_cuts .reserve(merge_num_upper / 1.5);
        gen_costs.reserve(merge_num_upper / 1.5);

        uint buffer[Cut::MAX_CUT_SIZE << 1] {0};

        for (int i0 = -1; i0 != cuts0.size(); ++i0) { Cut *c0 = i0 < 0 ? &t0 : cuts0[i0];
        for (int i1 = -1; i1 != cuts1.size(); ++i1) { Cut *c1 = i1 < 0 ? &t1 : cuts1[i1];
            // ++merge_idx;
            if (c0->size + c1->size > cfg.cut_size && __builtin_popcount(c0->sign | c1->sign) > cfg.cut_size)
                continue;
            std::memset(buffer, 0, sizeof(uint) * (cfg.cut_size * 2));
            uint *end = std::set_union(
                c0->leaves, c0->leaves + c0->size,
                c1->leaves, c1->leaves + c1->size,
                buffer
            );
            const int size = end - buffer;
            if (size > cfg.cut_size)
                continue;
            gen_cuts.push_back(allocate<Cut>(size, buffer, end));
            CutCost cost = compute_cost(gen_cuts.size() - 1, _mgr, gen_cuts.back(), cfg);
            gen_costs.push_back(cost);
        }}

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
        std::vector<Cut *> &cuts = _mgr.cut_set(id);
        for (int i = 0; i != gen_costs.size() && i != cfg.max_cut_num; ++i) {
            Cut *&cut = gen_cuts[gen_costs[i].idx];
            cuts.push_back(cut);
            cut = nullptr;
        }
        for (Cut *cut : gen_cuts) {
            deallocate(cut);
        }
        Cut *&best_cut = _mgr.best_cut(id);
        best_cut = cuts.front()->clone();
        // Set the node area/edge/arr info
        _mgr.area(id) = gen_costs.front().area / _mgr.num_est_ref(id);
        _mgr.edge(id) = gen_costs.front().edge / _mgr.num_est_ref(id);
    }
};

class mapping_flow
{
public:
    mapping_flow() = default;
   ~mapping_flow() = default;

    std::string_view name() const { return ""; }

    virtual void premap()  {}
    virtual void postmap() {}

    virtual void forward(mapper &)  {}
    virtual void backward(mapper &) {}
};

// generic area-flow mapping algo
class area_flow : public mapping_flow
{
public:
    area_flow() = default;
   ~area_flow() = default;

    virtual std::string_view name() const { return ""; }

    virtual void premap() {}
    virtual void postmap() {}
    virtual void forward(mapper &mgr)  {
    }
    virtual void backward(mapper &) {}
};



}
