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
#include <utility>
#include <vector>
#include <cassert>
#include <functional>
#include <deque>
#include <chrono>
#include <print>

#include "macros.hpp"
#include "basic.hpp"
#include "agdmap.hpp"

namespace abc {
    typedef struct Abc_Ntk_t_ Abc_Ntk_t;
}

namespace fox::supper {
// ========================================================================
// Cut
// ========================================================================
struct cut_data_w {
    uint8_t  sub_cuts[MAX_GATE_SIZE];
    uint8_t  nums; // num of sub-cut
    uint     leaves[0];

    Inline std::string operator*() const {
        std::string str = "<";
        for (int i = 0; i != nums; ++i)
            str += std::to_string(sub_cuts[i]) + " ";
        str += ">";
        return str;
    }
};

class Cut {
public:
    static constexpr std::size_t BIT_NUM_SIZE = 7;
    static constexpr std::size_t MAX_CUT_SIZE = (1 << BIT_NUM_SIZE) - 1;
    static constexpr std::size_t BIT_NUM_SIGN = std::numeric_limits<uint>::digits;

    enum data_t : uint8_t { KCUT = 0, WCUT };

    union {
        uint  sign;      // signature
        Area  a   ;      // area-cost. Used for wide-cut enumeration on-the-fly pruning
        struct {
            uint16 karea;   // number of internal k-cuts in this wide cut
            uint16 kedge;   // number of edges in this wide cut
        };
    };
    uint      size :  7; // cut-set size
    uint      crs  :  1; // compressed leaf array (first leaf -> id, diff21, diff32 ... )
    uint      dt   :  2; // data type
    uint      idx  : 22; // cut id, i.e., its position in cut list
    uint      fid;       // truth table (num. var <= 5) or functional id
private:
    uint      data[0];   // cut-data. Leaves or extened data.
public:

    Inline Cut &operator=(const Cut &cut) {
        if (&cut == this)
            return *this;
        std::memcpy((void *)this, &cut, cut.num_bytes());
        return *this;
    }

    /**
     * @brief Calculate the signature of the cut.
     * 
     * @return uint 
     */
    uint get_sign() const;

    /**
     * @brief Get the number of bytes used by the cut.
     */
    Inline std::size_t num_bytes() const {
        if (dt == data_t::KCUT)
            return sizeof(Cut) + sizeof(uint) * (size);
        else
            return sizeof(Cut) + sizeof(cut_data_w) + sizeof(uint) * (size);
    }

    Inline cut_data_w *wdata() {
        Assert(dt == data_t::WCUT);
        return reinterpret_cast<cut_data_w *>(data);
    }

    Inline cut_data_w *wdata() const {
        Assert(dt == data_t::WCUT);
        return const_cast<cut_data_w *>(reinterpret_cast<const cut_data_w *>(data));
    }

    Inline uint *begin() {
        switch (dt) {
        case data_t::KCUT:
            return data;
        case data_t::WCUT:
            return wdata()->leaves;
        }
        return nullptr;
    }

    Inline const uint *begin() const {
        switch (dt) {
        case data_t::KCUT:
            return data;
        case data_t::WCUT:
            return wdata()->leaves;
        }
        return nullptr;
    }

    // TODO: crs, end is unknown
    Inline uint *end() {
        switch (dt) {
        case data_t::KCUT:
            return data + size;
        case data_t::WCUT:
            return wdata()->leaves + size;
        }
        return nullptr;
    }

    Inline uint leaf(uint p) const { return begin()[p]; }

    Inline bool is_wcut() const { return dt == data_t::WCUT; }
    Inline bool is_kcut() const { return dt == data_t::KCUT; }

    // For cut copy
    static Inline Cut *copy(const Cut &cut) {
        void *ptr = std::malloc(cut.num_bytes());
        std::memcpy(ptr, &cut, cut.num_bytes());
        return static_cast<Cut *>(ptr);
    }

    // For general k-cut
    static Inline Cut *alloc_kcut(uint *begin, uint *end, Sign s, uint crs = 0) {
        Cut *ptr  = static_cast<Cut *>(std::calloc(1, sizeof(Cut) + sizeof(uint) * (end - begin)));
        ptr->sign = s;
        ptr->size = end - begin;
        ptr->crs  = crs;
        ptr->dt   = data_t::KCUT;
     // ptr->idx  = 0;
     // ptr->root = 0;
     // ptr->fid  = 0;
        std::copy(begin, end, ptr->data);
        return ptr;
    }

    // For trivial cut
    static Inline Cut *alloc_triv(uint id) {
        Cut *ptr  = static_cast<Cut *>(std::calloc(1, sizeof(Cut) + sizeof(uint) * 1));
        ptr->sign = SIGNATURE(id);
        ptr->size = 1;
     // ptr->crs  = 0;
        ptr->dt   = data_t::KCUT;
     // ptr->idx  = 0;
     // ptr->root = 0;
     // ptr->fid  = 0;
        ptr->data[0] = id;
        return ptr;
    }

    // For Agdmap wide cut
    static Inline Cut *alloc_wcut(uint *begin, uint *end) {
        Cut *ptr  = static_cast<Cut *>(std::calloc(1, sizeof(Cut) + sizeof(cut_data_w) + sizeof(uint) * (end - begin)));
     // ptr->sign = 0;
        ptr->size = end - begin;
     // ptr->crs  = 0;
        ptr->dt   = data_t::WCUT;
     // ptr->idx  = 0;
     // ptr->root = 0;
     // ptr->fid  = 0;
        cut_data_w *wdata = ptr->wdata();
        std::memset(wdata, 0, sizeof(cut_data_w));
        std::copy(begin, end, wdata->leaves);
        return ptr;
    }

    static Inline void dealloc(Cut *cut) {
        if (cut) [[likely]] {
            std::free(cut);
        }
    }

    Inline void add_sub_cut(uint sub_cut_idx) {
        wdata()->sub_cuts[wdata()->nums++] = sub_cut_idx;
    }

    Inline Area &area() { return a; }

    Inline Cut *next() const {
        return reinterpret_cast<Cut *>(reinterpret_cast<uintptr_t>(this) + num_bytes());
    }

    std::string operator*() const;
};

#define ForEachCutLeaf(C) \
    for (uint leaf = 0, i = 0; i != (C)->size && (leaf = (C)->begin()[i]); ++i)

enum class prune_mode_t {
    Unified,
    Separated
};


template <typename  T, prune_mode_t M>
class Prune {
    std::vector<std::vector<T>> _separated_set;
    std::vector<T>              _unified_set;
    std::vector<uint>           _capacity;
    std::function<bool(T, T)>   _cmp;
    Area                        _diff;

public:
    Prune(std::function<bool(T, T)> &&cmp) : _cmp(cmp), _diff(1.0) {}

    ~Prune() = default;

    Inline void set_diff(Area diff) {
        _diff = diff;
    }

    Inline void set_cap(std::vector<uint> &&list) {
        _capacity = std::move(list);
    }

    Inline void reset(std::size_t new_max_size, uint max_num = 4) {
        if constexpr (M == prune_mode_t::Unified) {
            _unified_set.clear();
            _unified_set.reserve(64);
        } else {
            _separated_set.clear();
            _separated_set.resize(new_max_size + 1);
            for (int i = 2; i != _separated_set.size(); ++i) {
                _separated_set[i].reserve(max_num);
            }
            _capacity.resize(new_max_size + 1, max_num);
        }
    }

    Inline void reset(std::vector<uint> &&cap) {
        // static_assert(M != prune_mode_t::Unified);
        _capacity = cap;
    }

    Inline void insert(T item) {
        if constexpr (M == prune_mode_t::Unified) {
            _unified_set.push_back(item);
            // Do not sort elements now, using nth element ranking when getting
            // std::ranges::sort(_unified_set, _cmp);
        } else {
            auto &vec = _separated_set[item->size];
            vec.push_back(item);
            // TODO: using self-implemented insert sorting for better performance
            std::ranges::sort(vec, _cmp);
            if (vec.size() > _capacity[item->size]) {
                // TODO: using self-implemented vector, pop_back is slow. --size is ok
                vec.pop_back();
            }
        }
    }

    void get(std::vector<T> &set, size_t n = 0)
    {
        set.reserve(n);
        if constexpr (M == prune_mode_t::Unified)
        {
            std::nth_element(
                _unified_set.begin(),
                _unified_set.begin() + n,
                _unified_set.end(),
                _cmp
            );
            std::copy(_unified_set.begin(), _unified_set.begin() + n, std::back_inserter(set));
            return;
        }
        else if constexpr (M == prune_mode_t::Separated) {
            Area last = kMaxArea;
            for (size_t sz = 2; sz != _separated_set.size(); ++sz)
            {
                auto &vec = _separated_set[sz];
                for (auto it = vec.rbegin(); it != vec.rend(); ++it) {
                    T    elem = *it;
                    Area curr = kMaxArea;
                    if constexpr (std::is_pointer_v<T>) {
                        curr = elem->area();
                    } else {
                        curr = elem.area();
                    }
                    if (curr > last + _diff) {
                        continue;
                    }
                    set.push_back(elem);
                    last = curr;
                }
            }
            std::ranges::reverse(set);
        }
    }

    void print() const;
};


// ========================================================================
// graph_t
// ========================================================================
class function_db {
    std::vector<word> _truth_tables;
public:
    function_db() = default;
    ~function_db() = default;

    word get_truth_table(uint id) {
        return _truth_tables[id];
    }
};

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
    function_db        *_db = nullptr;

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

#define ForEachGraphLut(mgr)                                               \
    for (int idx = (mgr).logic_begin(); idx != (mgr).logic_end(); ++idx)   \
        if ((mgr).num_est_ref(idx))

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
    float        epsilon     {0.005};

    bool area_mode()  const { return opt_target == target_t::AREA;  }
    bool delay_mode() const { return opt_target == target_t::DELAY; }
    // bool edge_mode()  const { return opt_target == target_t::EDGE;  }
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
    Time arr  {kMaxTime};
    uint size {0};
    uint idx  {0};

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
    Array<Gate *> _gates; // Simple gates

    std::vector<std::vector<Area>> _cuts_area;

    mutable Timer _timer;

    uint _num_area {0};
    uint _num_edge {0};
    uint _num_delay{0};

    uint64_t _stat_cut[3]{0};

public:
    friend class enumerate_cut;

    using CutSet = std::vector<Cut *>;

    mapper(uint max_node_num, uint num_pi = 0, uint num_po = 0)
    : graph_t(max_node_num, num_pi, num_po)
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
    }

    ~mapper() {
        for (Cut *cut : _best_cuts) {
            Cut::dealloc(cut);
        }
    }

    const Config &config() const { return _cfg; }

    void initialize();

    bool run_agdmap() const { return _cfg.map_impl == Config::AGDMAP; }

    // reset flags
    Inline void reset_est_ref () { std::fill(_est_ref .begin(), _est_ref .end(), 0       ); }
    Inline void reset_required() { std::fill(_required.begin(), _required.end(), kMaxTime); }

    // creators
    static mapper *create_from_aig (void       *ntk );
    static mapper *create_from_gia (void       *gia );
    static mapper *create_from_blif(const char *blif);

    template<Indexable T> Inline std::vector<Cut *> &cut_set(T n) { return _cuts[n]; }

    template<Indexable T> Inline Area  &area       (T n) { return _area[n];     }
    template<Indexable T> Inline Edge  &edge       (T n) { return _edge[n];     }
    template<Indexable T> Inline Time  &arrival    (T n) { return _arrival[n];  }
    template<Indexable T> Inline Time  &required   (T n) { return _required[n]; }
    template<Indexable T> Inline float &num_est_ref(T n) { return _est_ref[n];  }
    template<Indexable T> Inline uint  &num_ref    (T n) { return _int_ref[n];  }

    template<Indexable T> Inline Cut  *&best_cut(T n) {
        assert(n >= logic_begin());
        assert(n < logic_end());
        return _best_cuts[n];
    }

    template<Indexable T> Inline Area &cut_area(T n, uint idx) { return _cuts_area[n][idx]; }

    Inline uint &num_area()  { return _num_area;  }
    Inline uint &num_edge()  { return _num_edge;  }
    Inline uint &num_delay() { return _num_delay; }

    Inline const std::string &get_pi_name(uint idx) const { return _pi_names[idx]; }
    Inline const std::string &get_po_name(uint idx) const { return _po_names[idx]; }

    // uint64_t *get_stat_cut() { return _stat_cut; }
    uint64_t &num_merged    () { return _stat_cut[0]; }
    uint64_t &num_k_feasible() { return _stat_cut[1]; }
    uint64_t &num_stored    () { return _stat_cut[2]; }

    CutCost::cmp_res compare(const CutCost &lhs, const CutCost &rhs) {
        return _rank_fn(lhs, rhs, _cfg.epsilon);
    }

    // -- Agdmap related
    void  create_simple_gates(uint max_size);

    Gate *gate(uint id) { return _gates[id]; }

    Timer &timer() { return _timer; }

    graph_t *run_lut_mapping(const Config &cfg);

    graph_t *create_mapped_graph();

    void *create_abc_ntk_from_mapping(bool use_truth_table = true);

    Time calculate_delay();

    Area ref_mffc(Cut *cut);
    Edge rip_mffc(Cut *cut);

    void free_cuts() {
        ForEachGraphLogicNode(*this) {
            for (Cut *cut : _cuts[idx]) {
                Cut::dealloc(cut);
            }
            _cuts[idx].clear();
        }
    }

    CutCost compute_cut_cost(CutCostAlgo algo, Cut *cut);

    word compute_truth(Cut *cut, uint root) const;

    void print_node(uint id);
};

Cut *agd_decompose(mapper &mgr, CutCostAlgo algo, uint id, Cut *wcut, CutCost &cost);

// normal enumerate cut way
template <CutCostAlgo algo>
class CutEnumerator {
    mapper       &_mgr;
    const Config &_cfg;

    /**
     * @brief 
     * 
     * @param id 
     * @param kcuts 
     * @param costs 
     */
    void post_kcut_gen(uint id, std::vector<Cut *> &kcuts, std::vector<CutCost> &costs) {
        float epsilon = _cfg.epsilon;
        CutCost::rank_fn fn = CutCost::GetRankFn(_mgr.config().opt_target);
        auto fn_wrap = [epsilon, fn](const CutCost &lhs, const CutCost &rhs) -> bool {
            return fn(lhs, rhs, epsilon) == CutCost::cmp_res::LWIN;
        };

        if (costs.size() > _cfg.max_cut_num) {
            std::sort(costs.begin(), costs.end(), fn_wrap);
        } else{
            auto best_itr = std::min_element(costs.begin(), costs.end(), fn_wrap);
            std::iter_swap(costs.begin(), best_itr);
        }

        // mapping cost ranking back to cuts
        std::vector<Cut *> &cut_set = _mgr.cut_set(id);
        for (int i = 0; i != costs.size() && i != _cfg.max_cut_num; ++i) {
            Cut *&cut = kcuts[costs[i].idx];
            cut_set.push_back(cut);
            cut->idx = cut_set.size() - 1;
            cut = nullptr;
        }

        for (Cut *cut : kcuts) {
            Cut::dealloc(cut);
        }

        // Save the best cut
        *_mgr.best_cut(id) = *cut_set.front();

        // Set the node area/edge/arr info
        const float ratio = 1.0 / std::max(1.0f, float(_mgr.num_est_ref(id)));
        _mgr.area(id) = costs.front().area * ratio;
        _mgr.edge(id) = costs.front().edge * ratio;

        // Statics
        _mgr.num_stored() += cut_set.size();

        // create trivial cut
        Cut *triv_cut = Cut::alloc_triv(id);
        triv_cut->idx = cut_set.size();
        if constexpr (algo == CutCostAlgo::PRAETOR) {}
        cut_set.push_back(triv_cut);
    }

    std::vector<CutCost> compute_cut_cost(mapper &mgr, std::vector<Cut *> &cuts) {
        std::vector<CutCost> costs; costs.reserve(cuts.size());
        for (int i = 0; i != cuts.size(); ++i) {
            Cut *cut = cuts[i];
            // compute cut cost according to the algo
            CutCost cost;
            switch (algo) {
            case CutCostAlgo::FLOW: {
                cost.size = cut->size;
                cost.idx  = i;
                // area-flow/edge-flow
                ForEachCutLeaf(cut) {
                    cost.area += mgr.area(leaf);
                    cost.edge += mgr.edge(leaf);
                }
                // TODO: using cost library
                cost.area += 1.0;
                cost.edge += cut->size;
                break;
            }
            case CutCostAlgo::EXACT: {
                break;
            }
            case CutCostAlgo::PRAETOR: {
                break;
            }
            default:
                break;
            }
            costs.push_back(cost);
        }
        return costs;
    }

    void kcut_cut_enum(mapper &mgr, uint id) {
        const Config &cfg = mgr.config();

        Lit f0 = mgr[id][0];
        Lit f1 = mgr[id][1];

        const std::vector<Cut *> &cuts0 = mgr.cut_set(f0);
        const std::vector<Cut *> &cuts1 = mgr.cut_set(f1);

        uint  merge_num_upper = cuts0.size() * cuts1.size();
        uint  buffer[Cut::MAX_CUT_SIZE] {0};
        uint *end;

        mgr.num_merged() += merge_num_upper;

        std::vector<Cut *> cuts; cuts.reserve(merge_num_upper + 1);

        for (int i0 = 0; i0 != cuts0.size(); ++i0) { Cut *c0 = cuts0[i0];
        for (int i1 = 0; i1 != cuts1.size(); ++i1) { Cut *c1 = cuts1[i1];
            if (c0->size + c1->size > cfg.cut_size && popcount(c0->sign | c1->sign) > cfg.cut_size)
                continue;
            std::memset(buffer, 0, sizeof(uint) * (cfg.cut_size * 2));
            // TODO: using optimized merge
            if (end = set_union(buffer, c0->begin(), c0->end(), c1->begin(), c1->end(), cfg.cut_size); !end)
                continue;
            Assert(end - buffer <= cfg.cut_size);
            Cut *cut = Cut::alloc_kcut(buffer, end, c0->sign | c1->sign);
            cuts.push_back(cut);
        }} // end merge cuts

        mgr.num_k_feasible() += cuts.size();

        // Structure-based pruning

        // Restore the best cut from previous pass
        // if (Cut *best = mgr.best_cut(id); best) {
        //     cuts.push_back(best->clone());
        // }

        // Calculate the cut cost
        std::vector<CutCost> costs; costs.reserve(cuts.size());
        for (Cut *cut : cuts) {
            costs.push_back(mgr.compute_cut_cost(algo, cut));
        }

        // Cost-based cut pruning
        post_kcut_gen(id, cuts, costs);
    }

    // TODO: when gate size <= 8, using stack memory to allocate wide-cut.
    void wide_cut_enum(mapper &mgr, uint id) {
        const Config &cfg = mgr.config();

        std::function<bool(Cut *, Cut *)> cmp;
        if (cfg.opt_target == Config::target_t::AREA) {
            cmp = [](Cut *lhs, Cut *rhs) -> bool {
                return lhs->area() < rhs->area();
            };
        } else {
            Assert(0);
            cmp = [](Cut *lhs, Cut *rhs) -> bool {
                return lhs->size < rhs->size;
            };
        }

        // sort the gate inputs by area-cost increasing order ?

        std::vector<Cut *> *input_cut_sets[MAX_GATE_SIZE];
        const uint num_inputs = mgr.gate(id)->size();
        for (uint i = 0; i != num_inputs; ++i) {
            input_cut_sets[i] = &mgr.cut_set(mgr.gate(id)->input(i));
            // std::cout << "Input " << i << " cuts:\n";
            // for (Cut *cut : *input_cut_sets[i]) {
            //     std::cout << **cut << "\n";
            // }
        }

        std::vector<Cut *> curr_cut_sets(*input_cut_sets[0]);

        Prune<Cut *, prune_mode_t::Separated> prune(std::move(cmp));
        uint buffer[Cut::MAX_CUT_SIZE];
        uint n = 1;
        while (n < num_inputs) {
            prune.reset((n + 1) * cfg.lut_size, 4);
            for (Cut *c0 : curr_cut_sets)
            for (Cut *c1 : *input_cut_sets[n])
            {
                // merge the sub-cuts
                uint *end  = std::set_union(c0->begin(), c0->end(), c1->begin(), c1->end(), buffer);
                uint  size = end - buffer;
                // compute the area-cost for pruning
                // Or, just adding their area into a sum ?
                Cut *wcut = Cut::alloc_wcut(buffer, end);
                for (int i = 0; i != size; ++i) {
                    wcut->area() += mgr.area(buffer[i]);
                }
                // store the sub-cuts info
                if (n == 1) {
                    Assert(!c0->is_wcut() && !c1->is_wcut());
                    wcut->add_sub_cut(c0->idx);
                    wcut->add_sub_cut(c1->idx);
                } else {
                    Assert(c0->is_wcut() && !c1->is_wcut());
                    wcut->add_sub_cut(c1->idx);
                    for (int i = 0; i != c0->wdata()->nums; ++i)
                        wcut->add_sub_cut(c0->wdata()->sub_cuts[i]);
                }
                prune.insert(wcut);                
            }
            if (n != 1) {
                // Todo: reuse the cuts here
                for (Cut *cut : curr_cut_sets)
                    Cut::dealloc(cut);
            }
            curr_cut_sets.clear();
            prune.get(curr_cut_sets);
            // std::cout << "After merging input " << n << ", cut num = " << curr_cut_sets.size() << "\n";
            // for (Cut *cut : curr_cut_sets) {
            //     std::cout << **cut << "\n";
            // }
            ++n;
        }

        // Structure-based pruning
        // ?

        // Now curr_cut_sets contains all the final survived wide cuts
        // Decomposing them into k-feasible cuts
        std::vector<Cut *>   cuts;  cuts .reserve(curr_cut_sets.size());
        std::vector<CutCost> costs; costs.reserve(curr_cut_sets.size());

        // Cost-based cut pruning
        int idx = 0;
        for (Cut *wcut : curr_cut_sets) {
            CutCost cost;
            Cut *root_cut = agd_decompose(mgr, algo, id, wcut, cost);
            cuts .push_back(root_cut);
            cost .idx = idx++;
            costs.push_back(cost);
        }

        // ranking and prune the k-cuts
        post_kcut_gen(id, cuts, costs);
    }

public:
    CutEnumerator(mapper &mgr, uint id) : _mgr(mgr), _cfg(mgr.config()) {
        if (mgr.run_agdmap()) {
            if (Gate *g = mgr.gate(id)) {
                if (g->size() > 2) {
                    wide_cut_enum(mgr, id);
                } else {
                    kcut_cut_enum(mgr, id);
                }
            }
        } else {
            kcut_cut_enum(mgr, id);        
        }
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
        ForEachGraphLogicNode(_mgr) {
            CutEnumerator<CutCostAlgo::FLOW>(_mgr, idx);
        }
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
class Backword {
protected:
    mapper &_mgr;

    virtual Cut *get_best_cut(uint idx) {
        return _mgr.best_cut(idx);
    }

    virtual void reference_best_cuts() {
        ForEachGraphPoV(_mgr) {
            ++_mgr.num_est_ref(_mgr.get_po(idx)[0]);
        }

        ForEachGraphLogicNodeRev(_mgr) { 
            if (_mgr.num_est_ref(idx) == 0)
                continue;
            Cut *cut = _mgr.best_cut(idx);
            ForEachCutLeaf(cut) {
                ++_mgr.num_est_ref(leaf);
            }
            _mgr.num_area() ++;
            _mgr.num_edge() += cut->size;
        }
    }

    void propagate_required() {}

public:
    Backword(mapper &mgr) : _mgr(mgr) {}
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
};

class MappingPass {
    mapper &_mgr;
    std::unique_ptr<Forward>  _forward;
    std::unique_ptr<Backword> _backword;

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
        _backword = std::make_unique<Backword>(mgr);

        if (!mgr.config().first_pass) {
            // update the estimated reference number
            // do nothing, using previous pass's reference count for next cost compute
        }

        _mgr.timer().start("forward");
        ///////////////////////////////////////
        _forward ->impl();
        ///////////////////////////////////////
        _mgr.timer().stop ("forward");

        _mgr.timer().start("backward");
        ///////////////////////////////////////
        _backword->impl();
        ///////////////////////////////////////
        _mgr.timer().stop("backward");

        if (mgr.config().verbose) {
            TIME_STOP(T)
            std::println(std::cout, INFO1, pass, mgr.num_area(), mgr.num_edge(), Timer::formatted_time(cpu_T, 5),
                _mgr.num_merged(), _mgr.num_k_feasible(), _mgr.num_stored());
        }

        _mgr.timer().start("exact_imp");
        ///////////////////////////////////////
        improve_mapping_exactly(mgr);
        ///////////////////////////////////////
        _mgr.timer().stop("exact_imp");
    }

    ~MappingPass() {
        _mgr.free_cuts();
        _mgr.num_area()       = 0;
        _mgr.num_edge()       = 0;
        _mgr.num_merged()     = 0;
        _mgr.num_k_feasible() = 0;
        _mgr.num_stored()     = 0;
    }
};

}
