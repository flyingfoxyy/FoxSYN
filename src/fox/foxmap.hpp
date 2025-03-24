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

#include "utility.hpp"

#include "base/abc/abc.h"

namespace fox
{
namespace foxmap
{
constexpr uint32_t kMaxLutSize = 6;
constexpr uint32_t kMaxId      = (1 << 30);

class Param;
class Node;
class Cut;
class FoxMap;

using Area      = float;
using Edge      = float;
using Time     = uint32_t;
using Sign      = uint32_t;
using truth     = uint64_t;
using CutRanker = std::function<int(Cut *lhs, Cut *rhs)>;

/**
 * Optimization objects supported for foxmap
 */
enum class OptTarget { Timing,  Area, Routability };


enum class Algorithm { Praetor, Flow, Exact       };

#define GetSign(id) (1u << id % (sizeof(Sign) - 1))

//==----------------------------------------------------------------==//
//                              Param class                           //
//==----------------------------------------------------------------==//
class Param
{
public:
    /* the parammeters for technology mapping */
    OptTarget       tar               = OptTarget::Timing;
    Algorithm       algo              = Algorithm::Praetor;

    bool            verbose           = false;  // print log
    bool            always_enum_cut   = false;  // always enumerates cuts during each mapping pass

    std::size_t     required          = 0;      // the target delay of mapped LUT netlist
    std::size_t     lut_size          = 6;      // max LUT input size
    std::size_t     praetor_pass_num  = 4;      // the number of pass performing with effective area heuristic method
    std::size_t     flow_pass_num     = 4;      // the number of pass performing with area-flow      heuristic method
    std::size_t     exact_pass_num    = 4;      // the number of pass performing with exact area     heuristic method

    bool AreaDriven()  const { return tar == OptTarget::Area; }
    bool RouteDriven() const { return tar == OptTarget::Area; }
};

//==----------------------------------------------------------------==//
//                              Cut class                             //
//==----------------------------------------------------------------==//
struct Cut
{
    truth      tt    {0};
    Area       area  {0};
    Edge       edge  {0};
    Sign       sign  {0};
    Time      delay : 28;
    uint32_t   size  :  4;

    uint32_t   leaves[kMaxLutSize] {0};  // cut leafs

    Cut() : delay(0), size(1) {}

    // Cut(Area area, Edge edge, Sign sign, uint32_t size, uint32_t root)
    // : area(area), edge(edge), sign(sign), size(size)
    // {
    //     leaves[0] = root;
    // }

    ~Cut() = default;
    
    Cut(const Cut &cut) = default;
    Cut(Cut &&cut) noexcept = default;

    void ComputeCost(Cut *lhs, Cut *rhs, float fanoutl, float fanoutr, FoxMap *mapper);

    void ComputeTruth(Cut *lhs, Cut *rhs);

    bool MergeCut(Cut *lhs, Cut *rhs, int lut_size);

};

//==----------------------------------------------------------------==//
//                              Node class                            //
//==----------------------------------------------------------------==//
class Node 
{
public:
    enum class NodeType : uint { Const, PI, PO, And, None };

    static thread_local Node *_const_1;

private:
    /* graph info */
    uint      _fanin0   :  31;  // fanin0
    uint      _compl0   :   1;  // the complemented attribute for fanin0
    uint      _fanin1   :  31;  // fanin1
    uint      _compl1   :   1;  // the complemented attribute for fanin1
    NodeType  _type     :   4;  // node type
    /* mapping properties */
    uint      _num_cuts :  28;  // node type
    Time      _arr   {0};     // the arrival  time
    Time      _req   {0};     // the required time

    Cut      *_cut_set{nullptr};

public:
    /**
     * @brief Construct a new Node object
     * 
     * @Param abc_node the corresponding Abc Node
     */
    Node(Abc_Obj_t *abc_node);

    Node()
    {
        _fanin0 = kMaxId;
        _fanin1 = kMaxId;
        _compl0 = _compl1 = 0;
        _type   = NodeType::None;
    }

    ~Node()
    {
        if (_cut_set)
            delete[] _cut_set;
    }

    Node *GetFanin0() const { return Node::_const_1 + _fanin0; }
    Node *GetFanin1() const { return Node::_const_1 + _fanin1; }
    
    /**
     * @brief Get the Node Id
     * 
     * @return uint32_t 
     */
    uint32_t GetId() const
    {
        return this - Node::_const_1;
    }

    bool IsPi()  const { return _type == NodeType::PI;  }
    bool IsPo()  const { return _type == NodeType::PO;  }
    bool IsAnd() const { return _type == NodeType::And; }

    Edge GetEdge() const { return _cut_set[0].area; }
    Edge GetArea() const { return _cut_set[0].edge; }
    Time GetArr()  const { return _arr;  }
    Time GetReq()  const { return _req;  }

    /**
     * 
     */
    // uint &GetRefNum() { return _ref; }

    uint GetCutNum() { return _num_cuts; }

    Cut *GetCut(int idx) { return _cut_set + idx; }

    void CutEnum(FoxMap *mapper);
};


//==----------------------------------------------------------------==//
//                           Pruner class                             //
//==----------------------------------------------------------------==//
class Pruner
{
public:
    enum class mode
    {
        SIL,
        SILK,
        SL
    };

private:
    std::vector<std::vector<Cut *>> _idxed_list;
    std::vector<Cut *> _unified_list;
    std::vector<Cut *> _temp_lsit;

    mode _mode;

    Area _epsion;
    Area _min_area;

    CutRanker _ranker;

public:

    Cut *GetCandidate();

    /**
     * @brief pop the stored cuts into cut_set
     * 
     * @param cut_set 
     */
    int pop(Cut *&cut_set);

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
public:
    FoxMap *mapper {nullptr};

    std::vector<Cut *>     cuts;
    std::vector<uint32_t>  num_lut;

    uint32_t total_lut   {0};
    uint32_t total_edge  {0};
    uint32_t total_delay {0};

    bool operator<(const Solution &rhs);

};

//==----------------------------------------------------------------==//
//                              FoxMap class                          //
//==----------------------------------------------------------------==//
class FoxMap
{
    /* technology mapping property and flags */
    Param               *_map_param     {nullptr};   // parammeters of mapping algo
    Node                *_nodes         {nullptr};   // all nodes
    uint32_t             _num_nodes     {0};
    Abc_Ntk_t           *_pAig          {nullptr};   // AIG for mapping

    std::vector<Node *>  _prim_inputs;
    std::vector<Node *>  _prim_outputs;

    std::vector<uint>    _num_refs;
    std::vector<float>   _est_refs;

    double               _cpu_time       {0  };
    double               _wall_time      {0  };
    float                _lut_lib[10]    {1.0};

    Pruner    _pruner;
    Solution  _best_mapping;

    friend class Node;
    friend class Cut;
    friend class Solution;

public:
    FoxMap(Param *param, Abc_Ntk_t *pAig) : _map_param(param), _num_nodes(_pAig->vObjs->nSize + 1), _pAig(pAig) {}

    ~FoxMap()
    {
        delete []GetNode(0);
    }

    Abc_Ntk_t *MapToLut();

private:

    std::size_t NumPi()  const  { return _prim_inputs.size(); }
    std::size_t NumPo()  const  { return _prim_inputs.size(); }
    std::size_t NumAnd() const  { return _num_nodes - NumPi() - NumPo() - 2; }

    /**
     * Get the node 'idx'
     */
    Node *GetNode(int idx) const { return Node::_const_1 + idx; }

    /**
     * Get the estimated reference count for node idx
     */
    float GetEstRef(uint id) const { return _est_refs[id]; }

    /**
     * Get the mapping parameters
     */
    Param *GetParam() const { return _map_param; }

    /**
     * Get LUT area cost for different input size
     */
    Area GetLutCost(int size) const { return _lut_lib[size]; }

    /**
     * Generate mapped network according to final solution
     */
    Abc_Ntk_t *GenMappedNetwork(Solution *final);

    /**
     * Return the pruner for cut enumeration
     */
    Pruner &GetPruner() { return _pruner; }

    /**
     * @brief Perform a LUT mapping pass
     * 
     * @param algo 
     * @return Solution 
     */
    Solution *PerformMapping();

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
