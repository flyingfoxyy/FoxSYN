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

#include "base/abc/abc.h"

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
using RankFn = std::function<int(Cut *lhs, Cut *rhs, float epsilon)>;

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
    bool         verbose           = true;  // print log
    bool         always_enum_cut   = true ; // always enumerates cuts during each mapping pass

    std::size_t  lut_size          = 6;     // max LUT input size
    std::size_t  required          = 0;     // the target delay of mapped LUT netlist
    std::size_t  c_value           = 8;     // the cut number stored for each node (exclude trivial cut)
    std::size_t  praetor_pass_num  = 4;     // the number of pass performing with effective area heuristic method
    std::size_t  flow_pass_num     = 4;     // the number of pass performing with area-flow      heuristic method
    std::size_t  exact_pass_num    = 4;     // the number of pass performing with exact area     heuristic method

    bool TimingDriven() const { return tar == OptTarget::Timing;      }  // timing
    bool AreaDriven ()  const { return tar == OptTarget::Area;        }  // area/routability/timing
    bool RouteDriven()  const { return tar == OptTarget::Routability; }  // routability/area/timing
};

//==----------------------------------------------------------------==//
//                              Cut class                             //
//==----------------------------------------------------------------==//
struct Cut
{
    word   truth    ;   // truth table
    Area   area  {0};   // effective area / area-flow / exact area
    Edge   edge  {0};   // edge
    Sign   sign  {0};   // signature
    Time   arr   : 28;  // cut root arrival time
    uint   size  :  4;  // cut-size

    uint   leaves[kMaxLutSize] {0};  // cut leafs

    Cut() : truth(0xAAAAAAAAAAAAAAAA), arr(0), size(1) {}

    Cut(const Cut &cut) = default;
    Cut(Cut &&cut) noexcept = default;
    Cut &operator=(const Cut &cut) = default;

    /**
     * @brief Print this cut on consol
     * 
     */
    void Print();

    /**
     * @brief Compute the cost properties
     * 
     */
    void ComputeCost(Cut *lhs, Cut *rhs, float lhs_est_ref, float rhs_est_ref, FoxMap *mapper);

    /**
     * @brief Compute cut truth table
     * 
     */
    void ComputeTruth(Cut *lhs, Cut *rhs, int compl0, int compl1);

    /**
     * @brief Merge two sub-cuts to form cut-set
     * 
     * @return k-feasible or not
     */
    bool MergeCut(Cut *lhs, Cut *rhs, int k);

    bool IsValid() const { return leaves[0]; }

    /**
     * @brief Reference the MFFC of this cut
     * 
     * @param mapping current mapping solution
     * @param update update the solution on-the-fly
     * @return Area 
     */
    Area RefMFFC(FoxMap *mapper);

    /**
     * @brief Rip up the MFFC of this cut
     * 
     * @param mapping 
     * @param update 
     * @return Edge 
     */
    Edge RipMFFC(FoxMap *mapper);
};

//==----------------------------------------------------------------==//
//                              Node class                            //
//==----------------------------------------------------------------==//
class Node 
{
public:
    enum NodeType { Const, PI, PO, And, None };

    static thread_local Node *_const_1;

private:
    /* graph info */
    uint      _fanin0   :  31;  // fanin0
    uint      _compl0   :   1;  // the complemented attribute for fanin0
    uint      _fanin1   :  31;  // fanin1
    uint      _compl1   :   1;  // the complemented attribute for fanin1
    NodeType  _type     :   6;  // node type

    /* mapping properties */
    uint      _num_cuts :  26;  // node type
    uint      _required      ;  // required time
    uint      _num_ref{0}    ;  // reference count

    Cut      *_cut_set{nullptr};// cut-set
    Cut       _best_cut{};       // the best cut generated during last pass

public:
    /**
     * @brief Construct a new Node object
     * 
     * @Param abc_node the corresponding Abc Node
     */
    Node(Abc_Obj_t *abc_node);

    Node() : _fanin0(kMaxId), _compl0(0), _fanin1(kMaxId), _compl1(0), _type(NodeType::None), _num_cuts(0), _required(kMaxTime) {}

    ~Node()
    {
        delete[] _cut_set;
    }

    Node *GetFanin0()  const { return _fanin0 == kMaxId ? nullptr : Node::_const_1 + _fanin0; }
    Node *GetFanin1()  const { return _fanin1 == kMaxId ? nullptr : Node::_const_1 + _fanin1; }

    uint GetFanin0Id() const { return _fanin0; }
    uint GetFanin1Id() const { return _fanin1; }

    uint GetId() const { return this - Node::_const_1;    }
    uint &GetRefNum()  { return _num_ref;                 }

    bool IsPi()  const { return _type == NodeType::PI;    }
    bool IsPo()  const { return _type == NodeType::PO;    }
    bool IsAnd() const { return _type == NodeType::And;   }

    Area GetArea()       const { return _best_cut.area;   }
    Edge GetEdge()       const { return _best_cut.edge;   }
    Time GetArr()        const { return _best_cut.arr;    }

    uint GetCutNum()     const { return _num_cuts;                }
    Time GetRequired()   const { return _required;                }
    Cut *GetCut(int idx) const { return _cut_set + idx;           }
    Cut *GetTrivialCut() const { return _cut_set + _num_cuts - 1; }
    Cut *GetBestCut()          { return &_best_cut;               }

    void SetRequired(Time req) { _required = req;                 }

    void Print();
    
    /**
     * @brief Perform cut enumeration
     * 
     * @param mapper 
     */
    void CutEnum(FoxMap *mapper);

    /**
     * @brief Perform realtime best cut selection
     * 
     * @param mapping current mapping solution
     */
    Cut *SelectBestCut(Solution *mapping);
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

    PruneMode _mode {PruneMode::NONE};

    RankFn _rank_fn {};

    Area _epsilon   {0.001f  };
    Area _min_area  {kMaxArea};

    uint _temp_used_num    {0};

    Cut *_temp_cuts        {nullptr};

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
    void SetRankFn(const RankFn &fn) { _rank_fn = fn; }

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
    bool    _with_cover {false};

    mutable uint _max_arr  {0};

public:
    Solution(FoxMap *map, uint num, bool with_cover = true) : _mapper(map), _with_cover(with_cover)
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

    bool HasCover() const { return _with_cover; }

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
    struct LutCostLib
    {
        float area_cost[2][kMaxLutSize + 1]
        {
            {0, 1.00, 1.00, 1.00, 1.00, 1.00, 1.00},
            {0, 1.00, 1.00, 1.00, 1.00, 1.00, 1.00},
        };
        float edge_cost[2][kMaxLutSize + 1]
        {
            {0, 1.00, 2.00, 3.00, 4.00, 5.00, 6.00},
            {0, 1.00, 2.00, 3.00, 4.00, 5.00, 6.00},
        };

        /**
         * Sync LUT area cost according to ABC LUT library
         */
        void SyncUserLib();
    } _lut_lib;

    /* technology mapping property and flags */
    Param       *_map_param   {nullptr   };  // parammeters of mapping algo
    Algo         _algo        {Algo::Flow};  // algorithm during each pass
    Node        *_nodes       {nullptr   };  // all nodes
    uint32_t     _num_nodes   {0         };  // total node number
    Abc_Ntk_t   *_pAig        {nullptr   };  // AIG for mapping

    std::vector<Node *>  _prim_inputs;
    std::vector<Node *>  _prim_outputs;

    std::vector<uint>    _num_refs;  // real reference count in AIG
    std::vector<float>   _est_refs;  // estimated reference count for next pass

    double     _cpu_time       {0   };
    double     _wall_time      {0   };
    bool       _first_pass     {true};
    Time       _max_po_arr_time{0   };

    Prune      _prune;
    Solution  *_best_mapping   {nullptr};

    /* mapping runtime options */
    RankFn     _cut_rank_enu_fn{};
    RankFn     _cut_rank_sel_fn{};

    uint       _premap{0};

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
        delete[] GetNode(0);
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
     * Get the node 'idx'
     */
    static Node *GetNode(int idx) { return Node::_const_1 + idx; }

    /**
     * Get the estimated reference count for node idx
     */
    float GetEstRef(uint id) const { assert(_est_refs[id]); return _est_refs[id]; }

    /**
     * Get the mapping parameters
     */
    Param *GetParam() const { return _map_param; }

    uint GetNodeNum() const { return _num_nodes; }

    /**
     * @brief Get the algorithm for current mapping pass
     * 
     * @return Algo 
     */
    Algo GetAlgo() const { return _algo; }

    RankFn GetEnuRankFn() const { return _cut_rank_enu_fn; }
    RankFn GetSelRankFn() const { return _cut_rank_sel_fn; }

    /**
     * @brief Update the best mapping solution
     * 
     * @param new_mapping 
     */
    void UpdateMapping(Solution *new_mapping);

    /**
     * Get LUT area cost for different input size
     */
    Area GetLutAreaCost(int size) const { return _lut_lib.area_cost[0][size]; }

    /**
     * Get LUT edge cost for different input size
     */
    Edge GetLutEdgeCost(int size) const { return _lut_lib.edge_cost[0][size]; }

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

    void SetupLib() {}

    /**
     * @brief Improve mapping with MFFC rip up and exact remap
     * 
     * @param mapping 
     */
    void ImproveMapping(Solution *mapping);

    Time GetGlobalRequired();

    Solution *CreateTrivialMapping(bool with_cover = true);

    void ComputeRequiredTime(Solution *mapping);

    void PerformTimingDrivenPremapping();

    Solution *PerformGeneralMapping(Algo algo, OptTarget tar);

    /**
     * @brief Perform a LUT mapping pass
     * 
     * @param algo 
     * @return Solution 
     */
    Solution *PerformMapping(Algo algo);

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
