/*============================================================================\
|                                                                             |
| file:      node.cpp                                                         |
| author:    Longfei                                                          |
| purpose:   implementation of node                                           |
| version:   0.1                                                              |
| date:      2025-3-23                                                        |
\============================================================================*/

#include <algorithm>
#include <iostream>
#include <istream>

#include "foxmap.hpp"



namespace fox::supper
{

// inline static int popcount(uint32_t i) __attribute__((always_inline)) {
//     i = i - ((i >> 1) & 0x55555555);
//     i = (i & 0x33333333) + ((i >> 2) & 0x33333333);
//     i = ((i + (i >> 4)) & 0x0F0F0F0F);
//     return (i * (0x01010101)) >> 24;
// }

Node::Node(Abc_Obj_t *abc_node) : _mark(0), _num_cuts(0)
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
        _type    = NodeType::PI;
        _fanin0  = kMaxId;
        _fanin1  = kMaxId;
        _cut_set = new Cut[_num_cuts = 1];
        _cut_set->leaves[0] = Abc_ObjId(abc_node);
        _cut_set->sign = GetSign(Abc_ObjId(abc_node));
        _est_ref = _num_ref = Abc_ObjFanoutNum(abc_node);
        break;
    case ABC_OBJ_PO:
        _type   = NodeType::PO;
        _fanin0 = Abc_ObjFaninId0(abc_node);
        _fanin1 = kMaxId;
        _est_ref = _num_ref = Abc_ObjFanoutNum(abc_node);
        break;
    case ABC_OBJ_NODE:
        _type   = NodeType::And;
        _fanin0 = Abc_ObjFaninId0(abc_node);
        _fanin1 = Abc_ObjFaninId1(abc_node);
        _est_ref = _num_ref = Abc_ObjFanoutNum(abc_node);
        break;
    case ABC_OBJ_CONST1:
        _type   = NodeType::Const;
        _fanin0 = kMaxId;
        _fanin1 = kMaxId;
        _est_ref = _num_ref = Abc_ObjFanoutNum(abc_node);
        break;
    default:
        assert(0);
        break;
    }
}

void
Node::CutEnum(const FoxMap *mapper)
{
    if (IsPi())
    {
        GetTrivialCut()->area = mapper->GetAlgo() == Algo::Praetor ? Cut::GetAreaCost(GetTrivialCut()) : 0;
        GetTrivialCut()->edge = mapper->GetAlgo() == Algo::Praetor ? Cut::GetEdgeCost(GetTrivialCut()) : 0;
        return;
    }

    _est_ref = std::max(1u, _num_ref);

    CutSet cut_set(mapper->GetParam());
    cut_set.Reset();
    cut_set.SetRankFn(mapper->_cut_rank_enu_fn);
    cut_set.SetMode(CutSet::PruneMode::UL);

    Node *fanin0 = GetFanin0();
    Node *fanin1 = GetFanin1();

    const int k = mapper->GetParam()->lut_size;

    // rip-up current mapping
    if (_num_ref && !mapper->_premap && mapper->GetAlgo() == Algo::Exact)
        _best_cut.RipMFFC();

    // check in last best cut
    if (!mapper->_first_pass)
    {
        _best_cut.ComputeCost(mapper->GetAlgo());
        if (!mapper->_premap)
            cut_set.Push(&_best_cut);
    }

    // merge the cuts from fanins
    for (int i = 0; i != fanin0->GetCutNum(); ++i)
    {
        Cut *lhs = fanin0->GetCut(i);
        for (int m = 0; m != fanin1->GetCutNum(); ++m)
        {
            Cut *rhs = fanin1->GetCut(m);
            if (lhs->size + rhs->size > k && __builtin_popcount(lhs->sign | rhs->sign) > k)
                continue;
            Cut *cut = cut_set.GetCandidate();
            // check cut is k-feasible or not
            if (!cut->MergeCut(lhs, rhs, k))
                continue;
            cut->ComputeCost(mapper->GetAlgo(), this, lhs, rhs);
            if (!mapper->_premap && cut->arr > _required)
                continue;
            assert(cut->area > 0 && cut->area < kMaxArea);
            assert(cut->edge > 0 && cut->edge < kMaxArea);
            if (cut_set.Push(cut))
            {
                cut->sign = lhs->sign | rhs->sign;
                cut->ComputeTruth(lhs, rhs, _compl0, _compl1);
            }
        }
    }

    // pop the cuts
    _num_cuts = 1 + cut_set.Get(_cut_set, _num_cuts);
    assert(_num_cuts > 1);

    // update the best cut if on non-timing pass or (in timing pass but arrival time is ok)
    if (!mapper->_premap || _cut_set[0].arr <= _required)
        _best_cut = _cut_set[0];

    // reference the best cut into mapping
    if (_num_ref && !mapper->_premap && mapper->GetAlgo() == Algo::Exact)
        _best_cut.RefMFFC();

    // create trivial cut
    Cut *trival_cut = GetTrivialCut();
    trival_cut->leaves[0] = GetId();
    trival_cut->sign = GetSign(GetId());
    if (mapper->GetAlgo() == Algo::Praetor)
    {
        trival_cut->area = Cut::GetAreaCost(trival_cut) + GetBestCut()->area;
        trival_cut->edge = Cut::GetEdgeCost(trival_cut) + GetBestCut()->edge;
    }
}

void
Node::Print()
{
    printf("node: %d, cut num: %d\n", GetId(), _num_cuts);
    for (int i = 0; i != _num_cuts; ++i)
        GetCut(i)->Print();
}

}
