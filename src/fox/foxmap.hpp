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
namespace foxmap
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

    std::size_t  lut_size          = 6;  // max LUT input size
    std::size_t  required          = 0;  // the target delay of mapped LUT netlist
    std::size_t  c_value           = 8;  // the cut number stored for each node (exclude trivial cut)
    std::size_t  flow_pass_num     = 3;  // the number of pass performing with area-flow      heuristic method
    std::size_t  exact_pass_num    = 2;  // the number of pass performing with exact area     heuristic method

    bool TimingDriven() const { return tar == OptTarget::Timing;      }  // timing
    bool AreaDriven ()  const { return tar == OptTarget::Area;        }  // area/routability/timing
    bool RouteDriven()  const { return tar == OptTarget::Routability; }  // routability/area/timing
};

struct LutCostLib
{
    std::vector<Area> area_cost { 0, 1.00, 1.00, 1.00, 1.00, 1.00, 1.00 };
    std::vector<Edge> edge_cost { 0, 1.00, 2.00, 3.00, 4.00, 5.00, 6.00 };

    /**
     * Sync LUT area cost according to ABC LUT library
     */
    void SyncUserLib();
};

class MffcInfo;

//==----------------------------------------------------------------==//
//                              Cut class                             //
//==----------------------------------------------------------------==//
struct Cut
{
    static thread_local LutCostLib *s_lut_cost_lib;

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

    static Area GetAreaCost (const Cut *cut) { assert(s_lut_cost_lib); return s_lut_cost_lib->area_cost[cut->size]; }
    static Edge GetEdgeCost (const Cut *cut) { assert(s_lut_cost_lib); return s_lut_cost_lib->edge_cost[cut->size]; }
    static Time GetDelayCost(const Cut *cut) { return (Time)1; }

    /**
     * @brief Pretty print of cut
     * 
     */
    void Print();

    /**
     * @brief Compute the cost properties
     * 
     */
    void ComputeCost(Algo algo, Node *node = nullptr, Cut *lhs = nullptr, Cut *rhs = nullptr);

    /**
     * @brief Compute cut truth table according to two sub-cuts
     * 
     */
    void ComputeTruth(Cut *lhs, Cut *rhs, int compl0, int compl1);

    /**
     * @brief Compute cut truth table by reversed network visiting
     * 
     * @param root 
     */
    void ComputeTruth(Node *root);

    /**
     * @brief Merge two sub-cuts to form cut-set
     * 
     * @return k-feasible or not
     */
    bool MergeCut(Cut *lhs, Cut *rhs, int k);

    /**
     * @brief Test if this cut is a valid cut
     * 
     * @return true 
     * @return false 
     */
    bool IsValid() const { return size > 1 && leaves[0]; }

    /**
     * @brief Reference the MFFC of this cut
     * 
     * @return Area 
     */
    Area RefMFFC();

    /**
     * @brief Rip up the MFFC of this cut
     * 
     * @return Edge 
     */
    Edge RipMFFC();

    void RefMFFC(MffcInfo &info);
    void RipMFFC(MffcInfo &info);

    /**
     * @brief Compute the area/edge cost of cut MFFC
     * 
     * @return std::pair<Area, Edge> 
     */
    std::pair<Area, Edge> GetMFFCCostInfo();

    /**
     * @brief Compute the arrival time of this cut
     * 
     * @param map 
     * @return Time 
     */
    Time ComputeArrTime() const;

    /**
     * @brief Mark the nodes int the cone of this cut
     * 
     * @param root 
     * @param cone
     */
    void MarkCone(Node *root, std::vector<int> &cone);
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

    void AddNode(Cut *cut)
    {
        Area area = Cut::GetAreaCost(cut);
        Area edge = Cut::GetEdgeCost(cut);
        mffc_node_area.push_back(area);
        mffc_node_edge.push_back(edge);
        sum_area += area;
        sum_edge += edge;
    }


    int GetNodeNum() const { return mffc_node_area.size(); }

    Area GetArea() const { return sum_area; }
    Edge GetEdge() const { return sum_edge; }

    float GetAvgEdge() const { return sum_edge / GetNodeNum(); }

    float GetRatio() const
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

    static thread_local Node *s_const_1;

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

    Node *GetFanin0()  const { return _fanin0 == kMaxId ? nullptr : Node::s_const_1 + _fanin0; }
    Node *GetFanin1()  const { return _fanin1 == kMaxId ? nullptr : Node::s_const_1 + _fanin1; }

    uint GetFanin0Id() const { return _fanin0; }
    uint GetFanin1Id() const { return _fanin1; }

    uint GetId() const { return this - Node::s_const_1;   }
    uint &GetRefNum()  { return _num_ref;                 }

    bool IsPi()  const { return _type == NodeType::PI;    }
    bool IsPo()  const { return _type == NodeType::PO;    }
    bool IsAnd() const { return _type == NodeType::And;   }

    bool GetCompl0() const { return _compl0; }
    bool GetCompl1() const { return _compl1; }

    Area GetArea()       const { return _best_cut.area;   }
    Edge GetEdge()       const { return _best_cut.edge;   }
    Time GetArr()        const { return _best_cut.arr;    }

    uint GetCutNum()     const { return _num_cuts;                }
    uint GetMark()       const { return _mark;                    }
    Time GetRequired()   const { return _required;                }
    Cut *GetCut(int idx) const { return _cut_set + idx;           }
    Cut *GetTrivialCut() const { return _cut_set + _num_cuts - 1; }
    Cut *GetBestCut()          { return &_best_cut;               }
    word &GetTruth()           { return _truth;                   }
    float &GetEstRefNum()      { return _est_ref;                 }
    void SetRequired(Time req) { _required = req;                 }
    void SetMark(int mark)     { _mark = mark;                    }
    void SetBestCut(Cut *cut)  { if (cut != &_best_cut) _best_cut = *cut; }

    void Print();

    /**
     * @brief Get the node with id idx
     * 
     * @param idx 
     * @return Node* 
     */
    static Node *GetNode(int idx) { return Node::s_const_1 + idx; }

    /**
     * @brief Perform cut enumeration
     * 
     * @param mapper 
     */
    void CutEnum(FoxMap *mapper);
};


struct RankFnSet
{
    static constexpr float kEpsilon = 0.001;

    static int CmpCutArrSizeAreaEdge (Cut *lhs, Cut *rhs, float epsilon = kEpsilon);
    static int CmpCutArrAreaEdge     (Cut *lhs, Cut *rhs, float epsilon = kEpsilon);
    static int CmpCutArrEdgeArea     (Cut *lhs, Cut *rhs, float epsilon = kEpsilon);
    static int CmpCutAreaEdge        (Cut *lhs, Cut *rhs, float epsilon = kEpsilon);
    static int CmpCutEdgeArea        (Cut *lhs, Cut *rhs, float epsilon = kEpsilon);
};


//==----------------------------------------------------------------==//
//                            Prune class                             //
//==----------------------------------------------------------------==//
class Prune
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
    Prune(Param *param) : _temp_cuts(new Cut[(kMaxCutNum + 1) * (kMaxCutNum + 1)])
    {
        // initialize unified list
        _unified_list.resize(param->c_value + 1, nullptr); // the last one is not used
        // initialize indexed list
        const int k = param->lut_size;
        _indexed_list.resize(k + 1);
        for (int i = 0 ; i != _indexed_list.size(); ++i)
            _indexed_list[i].resize(_size_upper[k][i] + 1);
    }

    ~Prune()
    {
        delete[] _temp_cuts;
    }

    /**
     * @brief Set the mode for pruning
     * 
     * @param mode 
     */
    void SetMode(PruneMode mode) { _mode = mode; }

    /**
     * @brief Set the rank function for cuts
     * 
     * @param fn 
     */
    void SetRankFn(RankFn fn) { _rank_fn = fn; }

    /**
     * @brief Get the a general cut candidate
     * 
     * @return Cut* 
     */
    Cut *GetCandidate() { return _temp_cuts + _temp_used_num++; }

    /**
     * @brief Reset the status of this prune
     * 
     */
    void Reset();

    /**
     * @brief pop the stored cuts into cut_set
     * 
     * @param cut_set 
     */
    int Pop(Cut *&cut_set, uint capacity);

    /**
     * @brief push a cut into storage
     * 
     * @param cut 
     */
    bool Push(Cut *cut);
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
    FoxMap *GetMapper() const { return _mapper; }

    /**
     * @brief Get the cut solution for node
     * 
     */
    Cut *GetSol(uint node) const { return _cuts[node]; }

    uint GetLutNum()   const { return _sum_lut;  }
    uint GetEdgeNum()  const { return _sum_edge; }
    Time GetDelayNum() const;

    /**
     * @brief Get the reference count of node 'id'
     * 
     * @param id 
     * @return uint&
     */
    uint &GetRefCount(uint id) { return _ref_counter[id]; }

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
    void Add(uint id, Cut *cut);

    /**
     * @brief Remove solution for node 'id'
     * 
     */
    void Remove(uint id);

    /**
     * @brief Print mapping solution briefly
     * 
     */
    void Print(const char *algo, float time)
    {
        printf("%s: Delay = %3d  Area = %6d  Edge = %6d  Time = %.1f\n", algo, GetDelayNum(),
            GetLutNum(), GetEdgeNum(), time);
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

    LutCostLib _lut_lib{};

    bool       _first_pass     {true};
    Time       _max_po_arr_time{0   };

    Prune      _prune;
    Solution  *_best_mapping   {nullptr};

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
        _prune(param)
    {}

    ~FoxMap()
    {
        delete[] Node::GetNode(0);
        delete _best_mapping;
    }

    /**
     * @brief Map the network into look-up tables
     * 
     * @return Abc_Ntk_t* 
     */
    Abc_Ntk_t *MapToLut();

private:

    void Print();

    std::size_t NumPi()  const  { return _prim_inputs.size();  }
    std::size_t NumPo()  const  { return _prim_outputs.size(); }
    std::size_t NumAnd() const  { return _num_nodes - NumPi() - NumPo() - 2; }

    /**
     * Get the mapping parameters
     */
    Param *GetParam() const { return _map_param; }

    /**
     * @brief Get the node number
     * 
     * @return uint 
     */
    uint GetNodeNum() const { return _num_nodes; }

    /**
     * @brief Get the algorithm for current mapping pass
     * 
     * @return Algo 
     */
    Algo GetAlgo() const { return _algo; }

    /**
     * @brief Get the rank function for cut enumeration
     * 
     * @return RankFn 
     */
    RankFn GetEnuRankFn() const { return _cut_rank_enu_fn; }

    /**
     * @brief Get the rank function for cut selection
     * 
     * @return RankFn 
     */
    RankFn GetSelRankFn() const { return _cut_rank_sel_fn; }

    /**
     * @brief Update the best mapping solution
     * 
     * @param new_mapping 
     */
    void UpdateMapping(Solution *new_mapping);

    /**
     * @brief Get LUT delay cost for different input size
     * 
     * @param size 
     * @return Time 
     */
    Time GetLutDelayCost(int size) const { return 1; }

    /**
     * Generate mapped network according to final solution
     */
    Abc_Ntk_t *GenMappedNetwork(Solution *final);

    /**
     * Return the Prune for cut enumeration
     */
    Prune &GetPrune() { _prune.Reset(); _prune.SetRankFn(_cut_rank_enu_fn); return _prune; }

    /**
     * @brief Setup LUT cost library according to ABC read_lut command
     * 
     */
    void SetupLib() {}

    /**
     * @brief Get the global required time
     * 
     * @return Time 
     */
    Time GetGlobalRequired();

    /**
     * @brief Compute the required time and propagate them
     * 
     */
    void ComputeRequiredTime();

    /**
     * @brief Perform a general mapping pass according to given settings
     * 
     * @param algo 
     * @param fn 
     */
    void PerformGeneralMapping(Algo algo, RankFn fn);

    /**
     * @brief Try to improve the mapping by cuts reorder
     * 
     * @param algo 
     * @param fn 
     */
    void PerformExactImprovement(Algo algo, RankFn fn);

    /**
     * @brief Reference the best cuts
     * 
     */
    void ReferenceBestCuts();

    /**
     * @brief Create a Solution from current node refernce status
     * 
     * @return Solution* 
     */
    Solution *CreateSolFromCurrMap();

    /**
     * @brief Perform cut expansion for current mapping solution
     * 
     * @param lut_size 
     */
    void PerformCutExpansion(int lut_size);

    /**
     * @brief Expand cut towards PI, cut-set size cannot grow
     * 
     * @param node 
     * @return succeeded or not
     */
    bool NodeFaninCompact0(Node *node, std::vector<int> &front, std::vector<int> &visited);

    /**
     * @brief Expand cut towards PI, cut-set size grow but cannot exceed lut size
     * 
     * @param node 
     * @return succeeded or not
     */
    bool NodeFaninCompact1(Node *node, std::vector<int> &front, std::vector<int> &visited);

    /**
     * @brief Print current mapping round status
     * 
     * @param stage 
     * @param time 
     */
    void PrintMapping(const char *stage, float time)
    {
        printf("%s: Delay = %3d  Area = %6d  Edge = %6d  Time = %.1f\n", stage, _map_num_level,
            _map_num_lut, _map_num_edge, time);
    }

    /**
     * @brief Initialize the mapping graph from input AIG
     * 
     */
    void Initialize();
};

} // end namespace foxmap

/**
 * @brief execute queen map
 */
Abc_Ntk_t *PerformFoxMap(Abc_Ntk_t *Aig, foxmap::Param *param);

} // end namespace fox
