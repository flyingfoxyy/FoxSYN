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
Node::cut_enum(const FoxMap *mapper)
{
    if (is_pi())
    {
        get_trivial_cut()->area = mapper->get_algo() == Algo::Praetor ? Cut::get_area_cost(get_trivial_cut()) : 0;
        get_trivial_cut()->edge = mapper->get_algo() == Algo::Praetor ? Cut::get_edge_cost(get_trivial_cut()) : 0;
        return;
    }

    _est_ref = std::max(1u, _num_ref);

    CutSet cut_set(mapper->get_param());
    cut_set.reset();
    cut_set.set_rank_fn(mapper->_cut_rank_enu_fn);
    cut_set.set_mode(CutSet::PruneMode::UL);

    Node *fanin0 = get_fanin0();
    Node *fanin1 = get_fanin1();

    const int k = mapper->get_param()->lut_size;

    // rip-up current mapping
    if (_num_ref && !mapper->_premap && mapper->get_algo() == Algo::Exact)
        _best_cut.rip_mffc();

    // check in last best cut
    if (!mapper->_first_pass)
    {
        _best_cut.compute_cost(mapper->get_algo());
        if (!mapper->_premap)
            cut_set.push(&_best_cut);
    }

    // merge the cuts from fanins
    for (int i = 0; i != fanin0->get_cut_num(); ++i)
    {
        Cut *lhs = fanin0->get_cut(i);
        for (int m = 0; m != fanin1->get_cut_num(); ++m)
        {
            Cut *rhs = fanin1->get_cut(m);
            if (lhs->size + rhs->size > k && __builtin_popcount(lhs->sign | rhs->sign) > k)
                continue;
            Cut *cut = cut_set.get_candidate();
            // check cut is k-feasible or not
            if (!cut->merge_cut(lhs, rhs, k))
                continue;
            cut->compute_cost(mapper->get_algo(), this, lhs, rhs);
            if (!mapper->_premap && cut->arr > _required)
                continue;
            assert(cut->area > 0 && cut->area < kMaxArea);
            assert(cut->edge > 0 && cut->edge < kMaxArea);
            if (cut_set.push(cut))
            {
                cut->sign = lhs->sign | rhs->sign;
                cut->compute_truth(lhs, rhs, _compl0, _compl1);
            }
        }
    }

    // pop the cuts
    _num_cuts = 1 + cut_set.get(_cut_set, _num_cuts);
    assert(_num_cuts > 1);

    // update the best cut if on non-timing pass or (in timing pass but arrival time is ok)
    if (!mapper->_premap || _cut_set[0].arr <= _required)
        _best_cut = _cut_set[0];

    // reference the best cut into mapping
    if (_num_ref && !mapper->_premap && mapper->get_algo() == Algo::Exact)
        _best_cut.ref_mffc();

    // create trivial cut
    Cut *trival_cut = get_trivial_cut();
    trival_cut->leaves[0] = get_id();
    trival_cut->sign = GetSign(get_id());
    if (mapper->get_algo() == Algo::Praetor)
    {
        trival_cut->area = Cut::get_area_cost(trival_cut) + get_best_cut()->area;
        trival_cut->edge = Cut::get_edge_cost(trival_cut) + get_best_cut()->edge;
    }
}

void
Node::print()
{
    printf("node: %d, cut num: %d\n", get_id(), _num_cuts);
    for (int i = 0; i != _num_cuts; ++i)
        get_cut(i)->print();
}

}
