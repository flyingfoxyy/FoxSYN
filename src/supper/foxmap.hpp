/*---------------------------------------------------------------------------=\
|                                                                             |
| file:      foxmap.h                                                         |
| author:    longfei                                                          |
| purpose:   foxmap                                                           |
| version:   0.1                                                              |
| date:      2025-3-23                                                        |
\---------------------------------------------------------------------------=*/

#pragma once

#include <cassert>
#include <vector>
#include <functional>
#include <cstdint>
#include <cstring>

#include "utility.hpp"

#include "abc.hpp"

namespace fox
{
namespace supper
{
class Param;
class Node;
class Cut;
class FoxMap;
class Solution;

using Area   = float;
using Edge   = float;
using Time   = uint;
using Sign   = uint;
using word   = uint64_t;
using RankFn = int(*)(Cut *, Cut *, float);

constexpr uint kMaxLutSize = 6;
constexpr uint kMaxId      = 0x0FFFFFFF;
constexpr uint kMaxTime    = 0xFFFFFFFF;
constexpr uint kMaxCutNum  = 16;
constexpr Area kMaxArea    = 1234500000.0f;

/**
 * Optimization objects supported for foxmap
 */
enum class OptTarget { Timing,  Area, Routability };
enum class Algo      { Praetor, Flow, Exact       };

#define GetSign(id) (1u << id % (sizeof(Sign) - 1))

//==----------------------------------------------------------------==//
//                              Param class                           //
//==----------------------------------------------------------------==//
class Param
{
public:
    /* the parammeters for technology mapping */
    OptTarget    tar               = OptTarget::Timing;
    int          verbose           = 1;  // print log
    int          praetor_premap    = 0;  // always enumerates cuts during each mapping pass
    int          expand_cut        = 0;  // expand cuts
    int          parallel          = 0;  

    std::size_t  lut_size          = 6;  // max LUT input size
    std::size_t  required          = 0;  // the target delay of mapped LUT netlist
    std::size_t  c_value           = 8;  // the cut number stored for each node (exclude trivial cut)
    std::size_t  flow_pass_num     = 3;  // the number of pass performing with area-flow      heuristic method
    std::size_t  exact_pass_num    = 2;  // the number of pass performing with exact area     heuristic method

    bool timing_driven() const { return tar == OptTarget::Timing;      }  // timing
    bool area_driven ()  const { return tar == OptTarget::Area;        }  // area/routability/timing
    bool route_driven()  const { return tar == OptTarget::Routability; }  // routability/area/timing
};

struct LutCostLib
{
    std::vector<Area> area_cost { 0, 1.00, 1.00, 1.00, 1.00, 1.00, 1.00 };
    std::vector<Edge> edge_cost { 0, 1.00, 2.00, 3.00, 4.00, 5.00, 6.00 };

    /**
     * Sync LUT area cost according to ABC LUT library
     */
    void sync_user_lib();
};

class MffcInfo;

//==----------------------------------------------------------------==//
//                              Cut class                             //
//==----------------------------------------------------------------==//
struct Cut
{
    static LutCostLib *s_lut_cost_lib;

    word   truth     ; // truth table
    Area   area  {0} ; // effective area / area-flow / exact area
    Edge   edge  {0} ; // edge
    Sign   sign  {0} ; // signature
    Time   arr   : 28; // cut root arrival time
    uint   size  :  4; // cut-size
    float  ratio{0};

    uint   leaves[kMaxLutSize] {0};  // cut leafs

    Cut() : truth(0xAAAAAAAAAAAAAAAA), arr(0), size(1) {}

    Cut(const Cut &cut)            = default;
    Cut(Cut &&cut) noexcept        = default;
    Cut &operator=(const Cut &cut) = default;

    static Area get_area_cost (const Cut *cut) { assert(s_lut_cost_lib); return s_lut_cost_lib->area_cost[cut->size]; }
    static Edge get_edge_cost (const Cut *cut) { assert(s_lut_cost_lib); return s_lut_cost_lib->edge_cost[cut->size]; }
    static Time get_delay_cost(const Cut *cut) { return (Time)1; }

    /**
     * @brief Pretty print of cut
     * 
     */
    void print();

    /**
     * @brief Compute the cost properties
     * 
     */
    void compute_cost(Algo algo, Node *node = nullptr, Cut *lhs = nullptr, Cut *rhs = nullptr);

    /**
     * @brief Compute cut truth table according to two sub-cuts
     * 
     */
    void compute_truth(Cut *lhs, Cut *rhs, int compl0, int compl1);

    /**
     * @brief Compute cut truth table by reversed network visiting
     * 
     * @param root 
     */
    void compute_truth(Node *root);

    /**
     * @brief Merge two sub-cuts to form cut-set
     * 
     * @return k-feasible or not
     */
    bool merge_cut(Cut *lhs, Cut *rhs, int k);

    /**
     * @brief Test if this cut is a valid cut
     * 
     * @return true 
     * @return false 
     */
    bool is_valid() const { return size > 1 && leaves[0]; }

    /**
     * @brief Reference the MFFC of this cut
     * 
     * @return Area 
     */
    Area ref_mffc();

    /**
     * @brief Rip up the MFFC of this cut
     * 
     * @return Edge 
     */
    Edge rip_mffc();

    void ref_mffc(MffcInfo &info);
    void rip_mffc(MffcInfo &info);

    /**
     * @brief Compute the area/edge cost of cut MFFC
     * 
     * @return std::pair<Area, Edge> 
     */
    std::pair<Area, Edge> get_mffc_cost_info();

    /**
     * @brief Compute the arrival time of this cut
     * 
     * @param map 
     * @return Time 
     */
    Time compute_arr_time() const;

    /**
     * @brief Mark the nodes int the cone of this cut
     * 
     * @param root 
     * @param cone
     */
    void mark_cone(Node *root, std::vector<int> &cone);
};

class MffcInfo
{
    std::vector<Area> mffc_node_area;
    std::vector<Edge> mffc_node_edge;

    Area sum_area {0};
    Area sum_edge {0};

public:
    MffcInfo()
    {
        mffc_node_area.reserve(10);
        mffc_node_edge.reserve(10);
    }

    ~MffcInfo() = default;

    void add_node(Cut *cut)
    {
        Area area = Cut::get_area_cost(cut);
        Area edge = Cut::get_edge_cost(cut);
        mffc_node_area.push_back(area);
        mffc_node_edge.push_back(edge);
        sum_area += area;
        sum_edge += edge;
    }


    int get_node_num() const { return mffc_node_area.size(); }

    Area get_area() const { return sum_area; }
    Edge get_edge() const { return sum_edge; }

    float get_avg_edge() const { return sum_edge / get_node_num(); }

    float get_ratio() const
    {
        Edge sum = 0;
        for (Edge edge : mffc_node_edge)
            sum += edge * edge;
        return (float)sum / mffc_node_edge.size();
    }

};

//==----------------------------------------------------------------==//
//                              Node class                            //
//==----------------------------------------------------------------==//
class Node 
{
public:
    enum NodeType { Const, PI, PO, And, None };

    static Node *s_const_1;

private:
    /* graph info */
    uint      _fanin0   :  31;  // fanin0
    uint      _compl0   :   1;  // the complemented attribute for fanin0
    uint      _fanin1   :  31;  // fanin1
    uint      _compl1   :   1;  // the complemented attribute for fanin1
    NodeType  _type     :   6;  // node type

    /* mapping properties */
    uint      _mark     :  1 ;  // mark
    uint      _num_cuts :  25;  // number cuts
    uint      _required      ;  // required time
    uint      _num_ref {0}   ;  // reference count
    float     _est_ref {0}   ;  // estimated reference count
    word      _truth   {0}   ;  // truth table

    Cut      *_cut_set {nullptr};// cut-set
    Cut       _best_cut{};       // the best cut generated during last pass

public:
    /**
     * @brief Construct a new Node object
     * 
     * @Param abc_node the corresponding Abc Node
     */
    Node(Abc_Obj_t *abc_node);

    Node() : _fanin0(kMaxId), _compl0(0), _fanin1(kMaxId), _compl1(0), _type(NodeType::None), _mark(0), _num_cuts(0), _required(kMaxTime) {}

    ~Node()
    {
        delete[] _cut_set;
    }

    Node *get_fanin0()  const { return _fanin0 == kMaxId ? nullptr : Node::s_const_1 + _fanin0; }
    Node *get_fanin1()  const { return _fanin1 == kMaxId ? nullptr : Node::s_const_1 + _fanin1; }

    uint get_fanin0_id() const { return _fanin0; }
    uint get_fanin1_id() const { return _fanin1; }

    uint get_id() const { return this - Node::s_const_1;   }
    uint &get_ref_num()  { return _num_ref;                 }

    bool is_pi()  const { return _type == NodeType::PI;    }
    bool is_po()  const { return _type == NodeType::PO;    }
    bool is_and() const { return _type == NodeType::And;   }

    bool get_compl0() const { return _compl0; }
    bool get_compl1() const { return _compl1; }

    Area get_area()       const { return _best_cut.area;   }
    Edge get_edge()       const { return _best_cut.edge;   }
    Time get_arr()        const { return _best_cut.arr;    }

    uint get_cut_num()     const { return _num_cuts;                }
    uint get_mark()       const { return _mark;                    }
    Time get_required()   const { return _required;                }
    Cut *get_cut(int idx) const { return _cut_set + idx;           }
    Cut *get_trivial_cut() const { return _cut_set + _num_cuts - 1; }
    Cut *get_best_cut()          { return &_best_cut;               }
    word &get_truth()           { return _truth;                   }
    float &get_est_ref_num()      { return _est_ref;                 }
    void set_required(Time req) { _required = req;                 }
    void set_mark(int mark)     { _mark = mark;                    }
    void set_best_cut(Cut *cut)  { if (cut != &_best_cut) _best_cut = *cut; }

    void print();

    /**
     * @brief Get the node with id idx
     * 
     * @param idx 
     * @return Node* 
     */
    static Node *get_node(int idx) { return Node::s_const_1 + idx; }

    /**
     * @brief Perform cut enumeration
     * 
     * @param mapper 
     */
    void cut_enum(const FoxMap *mapper);
};


struct RankFnSet
{
    static constexpr float kEpsilon = 0.001;

    static int cmp_cut_arr_size_area_edge (Cut *lhs, Cut *rhs, float epsilon = kEpsilon);
    static int cmp_cut_arr_area_edge     (Cut *lhs, Cut *rhs, float epsilon = kEpsilon);
    static int cmp_cut_arr_edge_area     (Cut *lhs, Cut *rhs, float epsilon = kEpsilon);
    static int cmp_cut_area_edge        (Cut *lhs, Cut *rhs, float epsilon = kEpsilon);
    static int cmp_cut_edge_area        (Cut *lhs, Cut *rhs, float epsilon = kEpsilon);
};


//==----------------------------------------------------------------==//
//                            Prune class                             //
//==----------------------------------------------------------------==//
class CutSet
{
public:
    enum class PruneMode
    {
        NONE,
        UL,     // unified list
        IDLP,   // indexed list with area-size dominance pruning
        IDL,    // indexed list
    };

private:
    std::vector<std::vector<int>> _size_upper {
        {0},
        {0, 0},
        {0, 0, 0},
        {0, 0, 0, 0},
        {0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0},
        {0, 0, 1, 1, 1, 2, 3}
    };

    std::vector<std::vector<Cut *>> _indexed_list;
    std::vector<Cut *>              _unified_list;

    PruneMode _mode          {PruneMode::NONE};
    RankFn    _rank_fn       {nullptr };
    Cut      *_temp_cuts     {nullptr };
    Area      _epsilon       {0.001f  };
    Area      _min_area      {kMaxArea};
    uint      _temp_used_num {0};

public:
    CutSet(Param *param) : _temp_cuts(new Cut[(kMaxCutNum + 1) * (kMaxCutNum + 1)])
    {
        // initialize unified list
        _unified_list.resize(param->c_value + 1, nullptr); // the last one is not used
        // initialize indexed list
        const int k = param->lut_size;
        _indexed_list.resize(k + 1);
        for (int i = 0 ; i != _indexed_list.size(); ++i)
            _indexed_list[i].resize(_size_upper[k][i] + 1);
    }

    ~CutSet()
    {
        delete[] _temp_cuts;
    }

    /**
     * @brief Set the mode for pruning
     * 
     * @param mode 
     */
    void set_mode(PruneMode mode) { _mode = mode; }

    /**
     * @brief Set the rank function for cuts
     * 
     * @param fn 
     */
    void set_rank_fn(RankFn fn) { _rank_fn = fn; }

    /**
     * @brief Get the a general cut candidate
     * 
     * @return Cut* 
     */
    Cut *get_candidate() { return _temp_cuts + _temp_used_num++; }

    /**
     * @brief Reset the status of this prune
     * 
     */
    void reset();

    /**
     * @brief pop the stored cuts into cut_set
     * 
     * @param cut_set 
     */
    int get(Cut *&cut_set, uint capacity);

    /**
     * @brief push a cut into storage
     * 
     * @param cut 
     */
    bool push(Cut *cut);
};

//==----------------------------------------------------------------==//
//                           Solution class                           //
//==----------------------------------------------------------------==//
class Solution
{
    FoxMap *_mapper;

    std::vector<Cut *>  _cuts;
    std::vector<uint>   _ref_counter;

    uint    _num_lut[kMaxLutSize + 1] {0};
    uint    _sum_lut    {0};
    uint    _sum_edge   {0};

    mutable uint _max_arr  {0};

public:
    Solution(FoxMap *map, uint num, bool with_cover = true) : _mapper(map)
    {
        _cuts.resize(num);
        _ref_counter.resize(num);
    }

    ~Solution()
    {
        for (Cut *cut : _cuts)
            delete cut;
    }

    /**
     * @brief Get the mapper
     * 
     * @return FoxMap* 
     */
    FoxMap *get_mapper() const { return _mapper; }

    /**
     * @brief Get the cut solution for node
     * 
     */
    Cut *get_sol(uint node) const { return _cuts[node]; }

    uint get_lut_num()   const { return _sum_lut;  }
    uint get_edge_num()  const { return _sum_edge; }
    Time get_delay_num() const;

    /**
     * @brief Get the reference count of node 'id'
     * 
     * @param id 
     * @return uint&
     */
    uint &get_ref_count(uint id) { return _ref_counter[id]; }

    /**
     * @brief Compare this with rhs, return true if this has better qor
     * 
     * @param rhs 
     */
    bool operator<(const Solution &rhs);

    /**
     * @brief Add a solution for node 'id'
     * 
     */
    void add(uint id, Cut *cut);

    /**
     * @brief Remove solution for node 'id'
     * 
     */
    void remove(uint id);

    /**
     * @brief Print mapping solution briefly
     * 
     */
    void print(const char *algo, float time)
    {
        printf("%s: Delay = %3d  Area = %6d  Edge = %6d  Time = %.1f\n", algo, get_delay_num(),
            get_lut_num(), get_edge_num(), time);
    }
};

//==----------------------------------------------------------------==//
//                              FoxMap class                          //
//==----------------------------------------------------------------==//
class FoxMap
{
    /* technology mapping property and flags */
    Param       *_map_param   {nullptr   };  // parammeters of mapping algo
    Node        *_nodes       {nullptr   };  // all nodes
    uint         _num_nodes   {0         };  // total node number
    Abc_Ntk_t   *_pAig        {nullptr   };  // AIG for mapping

    std::vector<Node *>  _prim_inputs;
    std::vector<Node *>  _prim_outputs;

    std::vector<uint>    _num_refs;  // real reference count in AIG

    bool       _first_pass     {true};
    Time       _max_po_arr_time{0   };

    CutSet     _cut_set;
    Solution  *_best_mapping   {nullptr};

    LutCostLib _lut_lib;

    /* mapping runtime options */
    RankFn     _cut_rank_enu_fn{nullptr};
    RankFn     _cut_rank_sel_fn{nullptr};
    uint       _premap         {0};
    Algo       _algo           {Algo::Flow};

    /* current mapping solution info */
    uint       _map_num_lut    {0};
    uint       _map_num_level  {0};
    uint       _map_num_edge   {0};

    friend class Node;
    friend class Cut;
    friend class Solution;

public:
    FoxMap(Param *param, Abc_Ntk_t *pAig)
    :   _map_param(param),
        _pAig(pAig),
        _cut_set(param)
    {}

    ~FoxMap()
    {
        delete[] Node::get_node(0);
        delete _best_mapping;
    }

    /**
     * @brief Map the network into look-up tables
     * 
     * @return Abc_Ntk_t* 
     */
    Abc_Ntk_t *map_to_lut();

private:

    void print();

    std::size_t num_pi()  const  { return _prim_inputs.size();  }
    std::size_t num_po()  const  { return _prim_outputs.size(); }
    std::size_t num_and() const  { return _num_nodes - num_pi() - num_po() - 2; }

    /**
     * Get the mapping parameters
     */
    Param *get_param() const { return _map_param; }

    /**
     * @brief Get the node number
     * 
     * @return uint 
     */
    uint get_node_num() const { return _num_nodes; }

    /**
     * @brief Get the algorithm for current mapping pass
     * 
     * @return Algo 
     */
    Algo get_algo() const { return _algo; }

    /**
     * @brief Get the rank function for cut enumeration
     * 
     * @return RankFn 
     */
    RankFn get_enu_rank_fn() const { return _cut_rank_enu_fn; }

    /**
     * @brief Get the rank function for cut selection
     * 
     * @return RankFn 
     */
    RankFn get_sel_rank_fn() const { return _cut_rank_sel_fn; }

    /**
     * @brief Update the best mapping solution
     * 
     * @param new_mapping 
     */
    void update_mapping(Solution *new_mapping);

    /**
     * @brief Get LUT delay cost for different input size
     * 
     * @param size 
     * @return Time 
     */
    Time get_lut_delay_cost(int size) const { return 1; }

    /**
     * Generate mapped network according to final solution
     */
    Abc_Ntk_t *gen_mapped_network(Solution *final);

    /**
     * Return the CutSet for cut enumeration
     */
    CutSet &get_cut_set() { _cut_set.reset(); _cut_set.set_rank_fn(_cut_rank_enu_fn); return _cut_set; }

    /**
     * @brief Setup LUT cost library according to ABC read_lut command
     * 
     */
    void setup_lib() {}

    /**
     * @brief Get the global required time
     * 
     * @return Time 
     */
    Time get_global_required();

    /**
     * @brief Compute the required time and propagate them
     * 
     */
    void compute_required_time();

    /**
     * @brief Perform a general mapping pass according to given settings
     * 
     * @param algo 
     * @param fn 
     */
    void perform_general_mapping(Algo algo, RankFn fn);

    /**
     * @brief Try to improve the mapping by cuts reorder
     * 
     * @param algo 
     * @param fn 
     */
    void perform_exact_improvement(Algo algo, RankFn fn);

    /**
     * @brief Reference the best cuts
     * 
     */
    void reference_best_cuts();

    /**
     * @brief Create a Solution from current node refernce status
     * 
     * @return Solution* 
     */
    Solution *create_sol_from_curr_map();

    /**
     * @brief Perform cut expansion for current mapping solution
     * 
     * @param lut_size 
     */
    void perform_cut_expansion(int lut_size);

    /**
     * @brief Expand cut towards PI, cut-set size cannot grow
     * 
     * @param node 
     * @return succeeded or not
     */
    bool node_fanin_compact0(Node *node, std::vector<int> &front, std::vector<int> &visited);

    /**
     * @brief Expand cut towards PI, cut-set size grow but cannot exceed lut size
     * 
     * @param node 
     * @return succeeded or not
     */
    bool node_fanin_compact1(Node *node, std::vector<int> &front, std::vector<int> &visited);

    /**
     * @brief Print current mapping round status
     * 
     * @param stage 
     * @param time 
     */
    void print_mapping(const char *stage, float time)
    {
        printf("%s: Delay = %3d  Area = %6d  Edge = %6d  Time = %.1f\n", stage, _map_num_level,
            _map_num_lut, _map_num_edge, time);
    }

    /**
     * @brief Initialize the mapping graph from input AIG
     * 
     */
    void initialize();
};

} // end namespace foxmap

/**
 * @brief execute queen map
 */
Abc_Ntk_t *perform_supper_map(Abc_Ntk_t *Aig, supper::Param *param);

} // end namespace fox
