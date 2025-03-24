/*============================================================================\
|                                                                             |
| file:      foxmap.cpp                                                       |
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

bool
Cut::MergeCut(Cut *lhs, Cut *rhs, int lut_size)
{
    int nSize0 = lhs->size;
    int nSize1 = rhs->size;
    uint *pC0  = lhs->leaves;
    uint *pC1  = rhs->leaves;
    uint *pC   = leaves;
    int i, k, c, s;
    // the case of the largest cut sizes
    if ( nSize0 == lut_size && nSize1 == lut_size )
    {
        for ( i = 0; i < nSize0; i++ )
        {
            if ( pC0[i] != pC1[i] )
                return 0;
            pC[i] = pC0[i];
        }
        this->size = lut_size;
        return 1;
    }
    // compare two cuts with different numbers
    i = k = c = s = 0;
    if ( nSize0 == 0 ) goto FlushCut1;
    if ( nSize1 == 0 ) goto FlushCut0;
    while ( 1 )
    {
        if (c == lut_size)
            return 0;
        if (pC0[i] < pC1[k])
        {
            pC[c++] = pC0[i++];
            if (i >= nSize0)
                goto FlushCut1;
        }
        else if (pC0[i] > pC1[k])
        {
            pC[c++] = pC1[k++];
            if (k >= nSize1)
                goto FlushCut0;
        }
        else
        {
            pC[c++] = pC0[i++]; k++;
            if (i >= nSize0)
                goto FlushCut1;
            if (k >= nSize1)
                goto FlushCut0;
        }
    }

FlushCut0:
    if (c + nSize0 > lut_size + i)
        return 0;
    while (i < nSize0)
        pC[c++] = pC0[i++];
    this->size = c;
    return 1;

FlushCut1:
    if (c + nSize1 > lut_size + k)
        return 0;
    while (k < nSize1)
        pC[c++] = pC1[k++];
    this->size = c;
    return 1;
}

void
Cut::ComputeCost(Cut *lhs, Cut *rhs, float est_ref_l, float est_ref_r, FoxMap *mapper)
{
    Algorithm algo = mapper->GetParam()->algo;
    if (algo == Algorithm::Praetor)
    {
        area = mapper->GetLutCost(size);
        area += (lhs->area - mapper->GetLutCost(lhs->size)) / est_ref_l;
        area += (rhs->area - mapper->GetLutCost(rhs->size)) / est_ref_r;
        edge = static_cast<Edge>(size);
        for (int i = 0; i != size; ++i)
        {
            edge += mapper->GetNode(leaves[i])->GetEdge() / mapper->GetEstRef(leaves[i]);
        }
        // TODO: delay
    }
    else
    if (algo == Algorithm::Flow)
    {
        area = mapper->GetLutCost(size);
        edge = static_cast<Edge>(size);
        for (int i = 0; i != size; ++i)
        {
            Node *leaf = mapper->GetNode(leaves[i]);
            area += leaf->GetArea() / mapper->GetEstRef(leaf->GetId());
            edge += leaf->GetEdge() / mapper->GetEstRef(leaf->GetId());
        }
        // TODO: delay
    }
    else
    {

    }
}

Node::Node(Abc_Obj_t *abc_node)
{
    if (!abc_node)
    {
        _fanin0 = kMaxId;
        _fanin1 = kMaxId;
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
Node::CutEnum(FoxMap *mapper)
{
    if (!IsPi() && !IsAnd())
        return;

    if (IsPi())
    {
        _cut_set = new Cut[1];
        _cut_set->leaves[0] = GetId();
        _cut_set->sign = GetSign(GetId());
        return;
    }

    Pruner &pruner = mapper->GetPruner();

    Node *fanin0 = GetFanin0();
    Node *fanin1 = GetFanin1();

    float est_ref0 = mapper->GetEstRef(_fanin0);
    float est_ref1 = mapper->GetEstRef(_fanin1);

    const int k = mapper->GetParam()->lut_size;
    // merge the cuts from fanisns
    for (int i = 0; i != fanin0->GetCutNum(); ++i)
    {
        Cut *lhs = fanin0->GetCut(i);
        for (int m = 0; m != fanin1->GetCutNum(); ++m)
        {
            Cut *rhs = fanin1->GetCut(m);
            if (lhs->size + rhs->sign > k && fox::popcount(lhs->sign | rhs->sign) > k)
                continue;
            Cut *cut = pruner.GetCandidate();
            // check cut is k-feasible or not
            if (!cut->MergeCut(lhs, rhs, k))
                continue;
            cut->sign = lhs->sign | rhs->sign;
            cut->ComputeCost(lhs, rhs, est_ref0, est_ref1, mapper);
            if (pruner.push(cut))
                cut->ComputeTruth(lhs, rhs);
        }
    }

    // pop the cuts
    _num_cuts = pruner.pop(_cut_set);
    
    // create trivival cut
    Cut *trival_cut = _cut_set + _num_cuts - 1;
    if (mapper->GetParam()->algo == Algorithm::Praetor)
        trival_cut->area = GetCut(0)->area + mapper->GetLutCost(GetCut(0)->size);
    else
        trival_cut->area = GetCut(0)->area;
    trival_cut->edge = GetCut(0)->edge; // ? using minimal edge for propagation ?
}

void
FoxMap::Initialize()
{
    _prim_inputs.reserve(Abc_NtkPiNum(_pAig));
    _prim_outputs.reserve(Abc_NtkPoNum(_pAig));

    // creat mapping graph
    _num_nodes = _pAig->vObjs->nSize + 1;

    _num_refs.resize(_num_nodes);
    _est_refs.resize(_num_nodes);

    Node::_const_1 = new Node[_num_nodes];

    for (int i = 0; i != _pAig->vObjs->nSize; ++i)
    {
        if (Abc_Obj_t *pObj = Abc_NtkObj(_pAig, i); pObj)
        {
            Node *node = Node::_const_1 + i;
            pObj->pTemp = static_cast<void *>(new (node)Node(pObj));

            if (node->IsPi())
                _prim_inputs.push_back(node);
            else if (node->IsPo())
                _prim_outputs.push_back(node);
        
            int num_fanout = Abc_ObjFanoutNum(pObj);
            _num_refs[i] = num_fanout;
            _est_refs[i] = static_cast<float>(num_fanout);
        }
    }

    Abc_NtkCleanCopy(_pAig);
}

Solution *
FoxMap::PerformMapping()
{

}

Abc_Ntk_t *
FoxMap::MapToLut()
{
    Initialize();

    // report the graph info
    if (_map_param->verbose)
        printf("mapping graph -- Pi %ld, Po %ld, And %ld\n", NumPi(), NumPo(), NumAnd());

    // set up LUT library

    Solution *curr_mapping = nullptr;
    auto update_mapping = [&curr_mapping](Solution *new_mapping) -> void {
        if (!curr_mapping)
            curr_mapping = new_mapping;
        else if ((*new_mapping) < (*curr_mapping)) {
            delete curr_mapping;
            curr_mapping = new_mapping;
        } else {
            delete new_mapping;
        }
    };

    // perform mapping pass with different heuristics
    for (int i = 0; i != _map_param->praetor_pass_num; ++i)
    {
        Solution *mapping = PerformMapping();
        update_mapping(mapping);
    }

    for (int i = 0; i != _map_param->flow_pass_num; ++i)
    {
        Solution *mapping = PerformMapping();
        update_mapping(mapping);
    }

    // mapping solution
    return GenMappedNetwork(curr_mapping);
}

} // namespace foxmap

Abc_Ntk_t *
PerformFoxMap(Abc_Ntk_t *pAig, foxmap::Param *param)
{
    foxmap::Node::_const_1 = nullptr;
    foxmap::FoxMap mapper(param, pAig);

    return mapper.MapToLut();
}

} // namespace fox
