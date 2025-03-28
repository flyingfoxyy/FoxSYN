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
constexpr uint kMaxLutSize = 6;
constexpr uint kMaxId      = 123456789;
constexpr uint kMaxCutNum  = 16;
constexpr uint kMaxArea    = 1234500000.0f;

class Param;
class Node;
class Cut;
class FoxMap;
class Solution;

using Area  = float;
using Edge  = float;
using Time  = uint;
using Sign  = uint;
using word  = uint64_t;
using rank  = std::function<int(Cut *lhs, Cut *rhs, float epsilon)>;

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

    bool AreaDriven()  const { return tar == OptTarget::Area;        }  // area/routability/timing
    bool RouteDriven() const { return tar == OptTarget::Routability; }  // routability/area/timing
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

    /**
     * @brief Reference the MFFC of this cut
     * 
     * @param mapping current mapping solution
     * @param update update the solution on-the-fly
     * @return Area 
     */
    Area RefMFFC(Solution *mapping, bool update = false);

    /**
     * @brief Rip up the MFFC of this cut
     * 
     * @param mapping 
     * @param update 
     * @return Edge 
     */
    Edge RipMFFC(Solution *mapping, bool update = false);
};

//==----------------------------------------------------------------==//
//                              Node class                            //
//==----------------------------------------------------------------==//
class Node 
{
public:
    enum class NodeType { Const, PI, PO, And, None };

    static thread_local Node *_const_1;

private:
    /* graph info */
    uint      _fanin0   :  31;  // fanin0
    uint      _compl0   :   1;  // the complemented attribute for fanin0
    uint      _fanin1   :  31;  // fanin1
    uint      _compl1   :   1;  // the complemented attribute for fanin1
    NodeType  _type     :   6;  // node type
    /* mapping properties */
    uint      _num_cuts :  20;  // node type
    uint      _best_cut :   6;
    Time      _arr    {0};      // the arrival  time
    Time      _req    {0};      // the required time

    Cut      *_cut_set{nullptr};

public:
    /**
     * @brief Construct a new Node object
     * 
     * @Param abc_node the corresponding Abc Node
     */
    Node(Abc_Obj_t *abc_node);

    Node() : _fanin0(kMaxId), _compl0(0), _fanin1(kMaxId), _compl1(0), _type(NodeType::None), _num_cuts(0), _best_cut(0) {}

    ~Node()
    {
        delete[] _cut_set;
    }

    Node *GetFanin0()  const { return _fanin0 == kMaxId ? nullptr : Node::_const_1 + _fanin0; }
    Node *GetFanin1()  const { return _fanin1 == kMaxId ? nullptr : Node::_const_1 + _fanin1; }

    uint GetFanin0Id() const { return _fanin0; }
    uint GetFanin1Id() const { return _fanin1; }

    uint GetId() const { return this - Node::_const_1;}

    bool IsPi()  const { return _type == NodeType::PI;    }
    bool IsPo()  const { return _type == NodeType::PO;    }
    bool IsAnd() const { return _type == NodeType::And;   }

    Area GetArea()       const { return IsPi() ? 0 : _cut_set[0].area; }
    Edge GetEdge()       const { return IsPi() ? 0 : _cut_set[0].edge; }

    Time GetArr()        const { return _arr;                     }
    Time GetReq()        const { return _req;                     }

    uint GetCutNum()     const { return _num_cuts;                }
    Cut *GetCut(int idx) const { return _cut_set + idx;           }
    Cut *GetTrivialCut() const { return _cut_set + _num_cuts - 1; }

    Cut *GetBestCut()    const { return _cut_set + _best_cut;     }
    void SetBestCut(uint idx)  { _best_cut = idx;                 }

    void Print();
    
    /**
     * @brief Perform cut enumeration
     * 
     * @param mapper 
     */
    void CutEnum(FoxMap *mapper);
};


class CutRank
{
    static int CmpCutAreaEdge(Cut *lhs, Cut *rhs, float epsilon)
    {
        return 1;
    }

public:

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

    Area _epsilon   {0.001f  };
    Area _min_area  {kMaxArea};

    uint _temp_used_num    {0};
    Cut *_temp_cuts {nullptr};

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

    void SetMode(PruneMode mode) { _mode = mode; }

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
    std::vector<Cut *>  _cuts;
    std::vector<uint>   _ref_counter;

    uint    _num_lut[kMaxLutSize + 1] {0};
    uint    _sum_lut  {0};
    uint    _sum_edge {0};
    uint    _max_arr  {0};
    FoxMap *_mapper;

public:
    Solution(FoxMap *map, uint num) : _mapper(map)
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

    uint GetLutNum()  const { return _sum_lut;  }
    uint GetEdgeNum() const { return _sum_edge; }

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
        printf("%s: Area = %6d  Edge = %6d  Time = %.1f\n", algo, GetLutNum(), GetEdgeNum(), time);
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
    Param       *_map_param   {nullptr};        // parammeters of mapping algo
    Algo         _algo        {Algo::Praetor};  // algorithm during each pass
    Node        *_nodes       {nullptr};        // all nodes
    uint32_t     _num_nodes   {0};              // total node number
    Abc_Ntk_t   *_pAig        {nullptr};        // AIG for mapping

    std::vector<Node *>  _prim_inputs;
    std::vector<Node *>  _prim_outputs;

    std::vector<uint>    _num_refs;  // real reference count in AIG
    std::vector<float>   _est_refs;  // estimated reference count for next pass

    double     _cpu_time       {0   };
    double     _wall_time      {0   };
    bool       _first_pass     {true};

    Prune      _prune;
    Solution  *_best_mapping{nullptr};

    friend class Node;
    friend class Cut;
    friend class Solution;

public:
    FoxMap(Param *param, Abc_Ntk_t *pAig)
    :   _map_param(param),
        _pAig(pAig),
        _prune(param)
    {
        // _lut_lib.SyncUserLib();
    }

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

    /**
     * @brief Get the algorithm for current mapping pass
     * 
     * @return Algo 
     */
    Algo GetAlgo() const { return _algo; }

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
    Area GetLutEdgeCost(int size) const { return _lut_lib.edge_cost[0][size]; }

    /**
     * Generate mapped network according to final solution
     */
    Abc_Ntk_t *GenMappedNetwork(Solution *final);

    /**
     * Return the Prune for cut enumeration
     */
    Prune &GetPrune() { _prune.Reset(); return _prune; }

    /**
     * @brief Improve mapping with MFFC rip up and exact remap
     * 
     * @param mapping 
     */
    void ImproveMapping(Solution *mapping);

    /**
     * @brief Return the best cut according to algo
     * 
     */
    Cut *SelectBestCut(Solution *mapping, Cut *cut_set, int num, Algo algo);

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
