/*============================================================================\
|                                                                             |
| file:      foxmap.cpp                                                        |
| author:    Longfei                                                          |
| purpose:   implementation of foxmap                                         |
| version:   0.1                                                              |
| date:      2025-3-23                                                        |
\============================================================================*/

#include <iostream>

#include "foxmap.hpp"

namespace fox
{
namespace foxmap
{
thread_local Node *foxmap::Node::_const_1 = nullptr;

Node::Node(Abc_Obj_t *abc_node)
{
    if (!abc_node)
    {
        _fanin0 = _fanin0 = kMaxId;
        _compl0 = _compl1 = 0;
        _type   = NodeType::None;
        return;
    }

    _compl0 = abc_node->fCompl0;
    _compl1 = abc_node->fCompl1;

    switch (Abc_ObjType(abc_node))
    {
    case ABC_OBJ_PI:
        _type   = NodeType::PI;
        _fanin0 = kMaxId;
        _fanin1 = kMaxId;
        break;
    case ABC_OBJ_PO:
        _type   = NodeType::PI;
        _fanin0 = static_cast<Node *>(Abc_ObjFanin0(abc_node)->pTemp)->GetId();
        _fanin1 = kMaxId;
        break;
    case ABC_OBJ_NODE:
        _type   = NodeType::And;
        _fanin0 = static_cast<Node *>(Abc_ObjFanin0(abc_node)->pTemp)->GetId();
        _fanin1 = static_cast<Node *>(Abc_ObjFanin1(abc_node)->pTemp)->GetId();
        break;
    case ABC_OBJ_CONST1:
        _type   = NodeType::Const;
        _fanin0 = kMaxId;
        _fanin1 = kMaxId;
        break;
    default:
        assert(0);
        break;
    }
}

void
FoxMap::Initialize()
{
    _prim_inputs.reserve(Abc_NtkPiNum(_pAig));
    _prim_outputs.reserve(Abc_NtkPoNum(_pAig));

    // creat mapping graph
    Node *&nodes = Node::GetNodeSet();
    nodes = new Node[_pAig->vObjs->nSize + 1];

    for (int i = 0; i != _pAig->vObjs->nSize; ++i)
    {
        Abc_Obj_t *pObj = Abc_NtkObj(_pAig, i);
        Node *node = nodes + i;
        if (pObj) {
            pObj->pTemp = static_cast<void *>(new (node)Node(pObj));
            if (node->IsPi())
                _prim_inputs.push_back(node);
            else if (node->IsPo())
                _prim_outputs.push_back(node);
        }
    }
    Abc_NtkCleanCopy(_pAig);
}

Abc_Ntk_t *
FoxMap::MapToLut()
{
    Initialize();

    // report the graph info
    if (_map_param->verbose)
        printf("mapping graph -- Pi %ld, Po %ld, And %ld\n", NumPi(), NumPo(), NumAnd());

    // cut enumeration
    
    
    
    return nullptr;
}

} // namespace foxmap

Abc_Ntk_t *
PerformFoxMap(Abc_Ntk_t *pAig, foxmap::Param *param)
{
    foxmap::Node::GetNodeSet() = nullptr;
    foxmap::FoxMap mapper(param, pAig);

    return mapper.MapToLut();
}

} // namespace fox
