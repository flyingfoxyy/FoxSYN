/*---------------------------------------------------------------------------=\
|                                                                             |
| file:      queen.h                                                          |
| author:    Fox                                                              |
| purpose:   queen mapper header                                              |
| version:   0.1                                                              |
| date:      2022-5-26                                                        |
\---------------------------------------------------------------------------=*/

#pragma once

#include <cassert>
#include <vector>
#include <cstdint>

#include "base/abc/abc.h"

namespace fox
{
namespace foxmap
{
constexpr uint32_t kMaxLutSize = 6;
constexpr uint32_t kMaxId      = (1 << 30);

class FoxMap;
class Param;
class Node;
class Cut;

using Area  = float;
using Edge  = float;
using Delay = uint32_t;
using Sign  = uint32_t;
using truth = uint64_t;

enum class OptTarget { Timing,  Area, Routability };
enum class Algorithm { Praetor, Flow, Exact       };

//==----------------------------------------------------------------==//
//                              Param class                           //
//==----------------------------------------------------------------==//
class Param
{
public:
    /* the parammeters for technology mapping */
    OptTarget       tar             = OptTarget::Timing;
    Algorithm       algo            = Algorithm::Praetor;
    bool            verbose         = false;  // print log
    uint32_t        required        = 0;      // the target delay of mapped LUT netlist
    uint32_t        lut_size        = 6;      // max LUT input size
    uint32_t        flow__pass_num  = 4;      // the number of pass performing with effective area heuristic method
    uint32_t        exact_pass_num  = 4;      // the number of pass performing with area-flow      heuristic method
    uint32_t        praet_pass_num  = 4;      // the number of pass performing with effective area heuristic method

    bool AreaDriven()  const { return tar == OptTarget::Area; }
    bool RouteDriven() const { return tar == OptTarget::Area; }
};

//==----------------------------------------------------------------==//
//                              Cut class                             //
//==----------------------------------------------------------------==//
class Cut
{
public:
    truth      tt    {0};
    Area       area  {0};
    Edge       edge  {0};
    Sign       sign  {0};
    Delay      delay : 28;
    uint32_t   size  :  4;

    uint32_t   leaves[kMaxLutSize] {0};  // cut leafs

    void ComputeCost(Algorithm algo);
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
    /* netlist */
    uint32_t   _fanin0 :  31;  // fanin0
    uint32_t   _compl0 :   1;  // the complemented attribute for fanin0
    uint32_t   _fanin1 :  31;  // fanin1
    uint32_t   _compl1 :   1;  // the complemented attribute for fanin1
    NodeType   _type        ;  // node type
    
    /* mapping property */
    uint32_t   _ref   {0};     // reference count
    Area       _area  {0};     // area (effective, flow, exact)
    Edge       _edge  {0};     // edge flow
    Delay      _arr   {0};     // the arrival  time
    Delay      _req   {0};     // the required time

    Cut       *_cut_set{nullptr};

public:
    /**
     * @brief Construct a new Node object
     * 
     * @Param abc_node the corresponding Abc Node
     */
    Node(Abc_Obj_t *abc_node);

    Node()
    {
        _fanin0 = _fanin0 = kMaxId;
        _compl0 = _compl1 = 0;
        _type   = NodeType::None;
    }

    ~Node()
    {
        if (_cut_set)
            delete _cut_set;
    }

    static Node *&GetNodeSet()
    {
        return Node::_const_1;
    }
    
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

    void CutEnum(FoxMap &mapper);
};

//==----------------------------------------------------------------==//
//                           Solution class                           //
//==----------------------------------------------------------------==//
class Solution
{
    FoxMap *mapper {nullptr};

    std::vector<int8_t>    cut_idx;
    std::vector<uint32_t>  num_lut;

    uint32_t total_lut   {0};
    uint32_t total_edge  {0};
    uint32_t total_delay {0};

};

//==----------------------------------------------------------------==//
//                              FoxMap class                          //
//==----------------------------------------------------------------==//
class FoxMap
{
    /* technology mapping property and flags */
    Param     *_map_param {nullptr};   // parammeters of mapping algo
    Node      *_nodes     {nullptr};   // all nodes
    Abc_Ntk_t *_pAig      {nullptr};   // AIG for mapping

    std::vector<Node *> _prim_inputs;
    std::vector<Node *> _prim_outputs;

    double   _cpu__time  {0};
    double   _wall_time  {0};
    uint32_t _num_nodes  {0};

public:
    FoxMap(Param *param, Abc_Ntk_t *pAig) : _map_param(param), _pAig(pAig), _num_nodes(_pAig->vObjs->nSize + 1) {}

    ~FoxMap()
    {
        delete []Node::GetNodeSet();
    }

    Abc_Ntk_t *MapToLut();

private:

    std::size_t NumPi()  const { return _prim_inputs.size(); }
    std::size_t NumPo()  const { return _prim_inputs.size(); }
    std::size_t NumAnd() const { return _num_nodes - NumPi() - NumPo() - 2; }

    /**
     * @brief Perform a LUT mapping pass
     * 
     * @param algo 
     * @return Solution 
     */
    Solution PerformMapping();

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