/*============================================================================\
|                                                                             |
| file:      foxmap.cpp                                                       |
| author:    Longfei                                                          |
| purpose:   implementation of foxmap                                         |
| version:   0.1                                                              |
| date:      2025-3-23                                                        |
\============================================================================*/

#include <iostream>
#include <algorithm>

#include "foxmap.hpp"

#include "misc/util/utilTruth.h"
#include "base/main/main.h"
#include "map/if/if.h"

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
    if (nSize0 == lut_size && nSize1 == lut_size)
    {
        for (i = 0; i < nSize0; i++)
        {
            if (pC0[i] != pC1[i])
                return 0;
            pC[i] = pC0[i];
        }
        this->size = lut_size;
        return 1;
    }
    // compare two cuts with different numbers
    i = k = c = s = 0;
    if (nSize0 == 0)
        goto FlushCut1;
    if (nSize1 == 0)
        goto FlushCut0;
    while (1)
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
Cut::ComputeCost(Node *node, Cut *lhs, Cut *rhs, FoxMap *mapper)
{
    Algo algo = mapper->GetAlgo();
    if (algo == Algo::Flow)
    {
        area = mapper->GetLutAreaCost(size);
        edge = mapper->GetLutEdgeCost(size);
        for (int i = 0; i != size; ++i)
        {
            Node *leaf = mapper->GetNode(leaves[i]);
            area += leaf->GetArea() / leaf->GetEstRefNum();
            edge += leaf->GetEdge() / leaf->GetEstRefNum();
        }
    }
    else if (algo == Algo::Exact)
    {
        area = RefMFFC(mapper);
        edge = RipMFFC(mapper);
    }
    else
    {
        area = mapper->GetLutAreaCost(size);
        area += (lhs->area - mapper->GetLutAreaCost(lhs->size)) / node->GetFanin0()->GetEstRefNum();
        area += (rhs->area - mapper->GetLutAreaCost(rhs->size)) / node->GetFanin1()->GetEstRefNum();
        edge = mapper->GetLutEdgeCost(size);
        edge += (lhs->edge - mapper->GetLutEdgeCost(lhs->size)) / node->GetFanin0()->GetEstRefNum();
        edge += (rhs->edge - mapper->GetLutEdgeCost(rhs->size)) / node->GetFanin1()->GetEstRefNum();
    }

    // compute arrival time
    arr = 0;
    for (int i = 0; i != size; ++i)
        arr = std::max(mapper->GetNode(leaves[i])->GetArr(), arr);
    arr += mapper->GetLutDelayCost(size);
}

Area
Cut::RefMFFC(FoxMap *mapper)
{
    Area area = mapper->GetLutAreaCost(size);
    for (int i = 0; i != size; ++i)
    {
        Node *node = FoxMap::GetNode(leaves[i]);
        if (node->GetRefNum()++ > 0 || !node->IsAnd())
            continue;
        area += node->GetBestCut()->RefMFFC(mapper);
    }
    return area;
}

Edge
Cut::RipMFFC(FoxMap *mapper)
{
    assert(IsValid());
    Edge edge = mapper->GetLutEdgeCost(size);
    for (int i = 0; i != size; ++i)
    {
        Node *node = FoxMap::GetNode(leaves[i]);
        assert(node->GetRefNum() > 0);
        if (--node->GetRefNum() > 0 || !node->IsAnd())
            continue;
        edge += node->GetBestCut()->RipMFFC(mapper);
    }
    return edge;
}

void
Cut::MarkCone(Node *node, std::vector<int> &cone)
{
    if (node->GetMark())
        return;
    assert(node->IsAnd());
    MarkCone(node->GetFanin0(), cone);
    MarkCone(node->GetFanin1(), cone);
    cone.push_back(node->GetId());
    node->SetMark(1);
}

void
Cut::ComputeTruth(Cut *lhs, Cut *rhs, int compl0, int compl1)
{
    auto TtExpand = [](word * pTruth0, int nVars, Cut *sub, Cut *cut) -> void
    {
        int i, k;
        for (i = cut->size - 1, k = sub->size - 1; i >= 0 && k >= 0; i--)
        {
            if (cut->leaves[i] > sub->leaves[k])
                continue;
            // assert(cut->leaves[i]->id() == sub->leaves[k]->id());
            if (k < i)
                Abc_TtSwapVars(pTruth0, nVars, k, i);
            k--;
        }
        assert( k == -1 );
    };

    word truth0 = lhs->truth;
    word truth1 = rhs->truth;
    TtExpand(&truth0, size, lhs, this);
    TtExpand(&truth1, size, rhs, this);
    if (compl0)
        truth0 = ~truth0;
    if (compl1)
        truth1 = ~truth1;
    truth = truth0 & truth1;
}

void
Cut::Print()
{
    printf("{Arr %d, Area %.1f, Edge %.1f, cut-set {%4d, %4d, %4d, %4d, %4d, %4d}}\n",
        arr, area, edge, leaves[0], leaves[1], leaves[2], leaves[3], leaves[4], leaves[5]);
}

int
RankFnSet::CmpCutArrSizeAreaEdge(Cut *lhs, Cut *rhs, float epsilon)
{
    if (lhs->arr + epsilon < rhs->arr)
        return 1;
    if (lhs->arr - epsilon > rhs->arr)
        return -1;
    if (lhs->size < rhs->size)
        return 1;
    if (lhs->size > rhs->size)
        return -1;
    if (lhs->area + epsilon < rhs->area)
        return 1;
    if (lhs->area - epsilon > rhs->area)
        return -1;
    if (lhs->edge + epsilon < rhs->edge)
        return 1;
    if (lhs->edge - epsilon > rhs->edge)
        return -1;
    return 0;
}

int
RankFnSet::CmpCutArrAreaEdge(Cut *lhs, Cut *rhs, float epsilon)
{
    if (lhs->arr + epsilon < rhs->arr)
        return 1;
    if (lhs->arr - epsilon > rhs->arr)
        return -1;
    if (lhs->area + epsilon < rhs->area)
        return 1;
    if (lhs->area - epsilon > rhs->area)
        return -1;
    if (lhs->edge + epsilon < rhs->edge)
        return 1;
    if (lhs->edge - epsilon > rhs->edge)
        return -1;
    return 0;
}

int
RankFnSet::CmpCutArrEdgeArea(Cut *lhs, Cut *rhs, float epsilon)
{
    if (lhs->arr + epsilon < rhs->arr)
        return 1;
    if (lhs->arr - epsilon > rhs->arr)
        return -1;
    if (lhs->edge + epsilon < rhs->edge)
        return 1;
    if (lhs->edge - epsilon > rhs->edge)
        return -1;
    if (lhs->area + epsilon < rhs->area)
        return 1;
    if (lhs->area - epsilon > rhs->area)
        return -1;
    return 0;
}

int
RankFnSet::CmpCutAreaEdge(Cut *lhs, Cut *rhs, float epsilon)
{
    if (lhs->area + epsilon < rhs->area)
        return 1;
    if (lhs->area - epsilon > rhs->area)
        return -1;
    if (lhs->edge + epsilon < rhs->edge)
        return 1;
    if (lhs->edge - epsilon > rhs->edge)
        return -1;
    if (lhs->size < rhs->size)
        return 1;
    if (lhs->size > rhs->size)
        return -1;
    return 0;
}

int
RankFnSet::CmpCutEdgeArea(Cut *lhs, Cut *rhs, float epsilon)
{
    if (lhs->edge + epsilon < rhs->edge)
        return 1;
    if (lhs->edge - epsilon > rhs->edge)
        return -1;
    if (lhs->area + epsilon < rhs->area)
        return 1;
    if (lhs->area - epsilon > rhs->area)
        return -1;
    if (lhs->size < rhs->size)
        return 1;
    if (lhs->size > rhs->size)
        return -1;
    return 0;
}

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
Node::CutEnum(FoxMap *mapper)
{
    if (IsPi())
    {
        GetTrivialCut()->area = mapper->GetAlgo() == Algo::Praetor ? mapper->GetLutAreaCost(1) : 0;
        GetTrivialCut()->edge = mapper->GetAlgo() == Algo::Praetor ? mapper->GetLutEdgeCost(1) : 0;
        return;
    }

    // update node estimated reference count
    // if (mapper->_premap)
        _est_ref = std::max(1u, _num_ref);
    // else if (mapper->GetAlgo() == Algo::Flow)
    //     _est_ref = (_est_ref + 4 * _num_ref) / 5.00;

    Prune &prune = mapper->GetPrune();
    Node *fanin0 = GetFanin0();
    Node *fanin1 = GetFanin1();

    const int k = mapper->GetParam()->lut_size;

    // rip-up current mapping
    if (_num_ref && !mapper->_premap && mapper->GetAlgo() == Algo::Exact)
        _best_cut.RipMFFC(mapper);

    // check in last best cut
    if (!mapper->_first_pass)
    {
        _best_cut.ComputeCost(this, nullptr, nullptr, mapper);
        if (!mapper->_premap)
            prune.Push(&_best_cut);
    }

    // merge the cuts from fanins
    for (int i = 0; i != fanin0->GetCutNum(); ++i)
    {
        Cut *lhs = fanin0->GetCut(i);
        for (int m = 0; m != fanin1->GetCutNum(); ++m)
        {
            Cut *rhs = fanin1->GetCut(m);
            if (lhs->size + rhs->size > k && fox::popcount(lhs->sign | rhs->sign) > k)
                continue;
            Cut *cut = prune.GetCandidate();
            // check cut is k-feasible or not
            if (!cut->MergeCut(lhs, rhs, k))
                continue;
            cut->ComputeCost(this, lhs, rhs, mapper);
            if (!mapper->_premap && cut->arr > _required)
                continue;
            assert(cut->area > 0 && cut->area < kMaxArea);
            assert(cut->edge > 0 && cut->edge < kMaxArea);
            if (prune.Push(cut))
            {
                cut->sign = lhs->sign | rhs->sign;
                cut->ComputeTruth(lhs, rhs, _compl0, _compl1);
            }
        }
    }

    // pop the cuts
    _num_cuts = 1 + prune.Pop(_cut_set, _num_cuts);
    assert(_num_cuts > 1);

    // update the best cut if on non-timing pass or (in timing pass but arrival time is ok)
    if (!mapper->_premap || _cut_set[0].arr <= _required)
        _best_cut = _cut_set[0];

    // reference the best cut into mapping
    if (_num_ref && !mapper->_premap && mapper->GetAlgo() == Algo::Exact)
        _best_cut.RefMFFC(mapper);

    // create trivial cut
    Cut *trival_cut = GetTrivialCut();
    trival_cut->leaves[0] = GetId();
    trival_cut->sign = GetSign(GetId());
    if (mapper->GetAlgo() == Algo::Praetor)
    {
        trival_cut->area = mapper->GetLutAreaCost(1) + GetBestCut()->area;
        trival_cut->edge = mapper->GetLutEdgeCost(1) + GetBestCut()->edge;
    }
}

void
Node::Print()
{
    printf("node: %d, cut num: %d\n", GetId(), _num_cuts);
    for (int i = 0; i != _num_cuts; ++i)
        GetCut(i)->Print();
}

int 
Prune::Pop(Cut *&cut_set, uint capacity)
{
    // a cut-set to store cuts in temp
    std::vector<Cut *> cuts;
    cuts.reserve(kMaxCutNum);

    if (_mode == PruneMode::UL)
    {
        for (auto it = _unified_list.begin(); it != _unified_list.end(); ++it)
        {
            if (*it)
                cuts.push_back(*it);
            else
                break;
        }
    }
    else if (_mode == PruneMode::IDLP)
    {
        Area prev_one_area = kMaxArea;
        for (int i = 2; i != _indexed_list.size(); ++i)
        {
            std::vector<Cut *> &list = _indexed_list[i];
            if (!list.front())
                continue;
            assert(!list.back());
            for (int m = list.size() - 2; m != -1; --m)
            {
                Cut *cut = list[m];
                if (cut && cut->area < _min_area + 1.000 && cut->area < prev_one_area)
                {
                    cuts.push_back(cut);
                    prev_one_area = cut->area;
                }
            }
        }
        std::reverse(cuts.begin(), cuts.end());
    }

    if (capacity < cuts.size() + 1)
    {
        delete[] cut_set;
        cut_set = new Cut[cuts.size() + 1];        
    }
    else
        std::fill(cut_set, cut_set + capacity, Cut{});

    // copy candidates of cuts into cut_set
    for (int i = 0; i != cuts.size(); ++i)
        cut_set[i] = *cuts[i];

    return cuts.size();
}

bool
Prune::Push(Cut *cut)
{
    if (_mode == PruneMode::IDLP && cut->area > _min_area + 1.000)
        return false;

    _min_area = std::min(_min_area, cut->area);

    std::vector<Cut *> &cut_list = _mode == PruneMode::UL ? _unified_list : _indexed_list[cut->size];

    for (int i = 0; i != cut_list.size(); ++i)
    {
        // rank to the last one
        if (!cut_list[i])
        {
            if (i == cut_list.size() - 1)
                return false;
            cut_list[i] = cut;
            return true;
        }
        // compare with current cut
        const int cmp_res = _rank_fn(cut, cut_list[i], _epsilon);
        // win
        if (cmp_res == 1)
        {
            for (int m = cut_list.size() - 1; m != i; --m)
                cut_list[m] = cut_list[m - 1];
            cut_list[i] = cut;
            if (cut_list.back())
                cut_list.back() = nullptr;
            return true;
        }
        // same quality, only keep one
        else if (cmp_res == 0)
            return false;
    }

    assert(0 && "should not arrival here");
    return true;
}

void
Prune::Reset()
{
    std::fill(_temp_cuts, _temp_cuts + _temp_used_num, Cut{});
    std::fill(_unified_list.begin(), _unified_list.end(), nullptr);
    _min_area = kMaxArea;
    _temp_used_num = 0;
    for (auto &&cut_set: _indexed_list)
        std::fill(cut_set.begin(), cut_set.end(), nullptr);
}

bool
Solution::operator<(const Solution& rhs)
{
    if (_mapper->GetParam()->tar == OptTarget::Timing)
    {
        if (GetDelayNum() < rhs.GetDelayNum())
            return true;
        if (GetDelayNum() > rhs.GetDelayNum())
            return false;
    }
    if (GetLutNum() < rhs.GetLutNum())
        return true;
    if (GetLutNum() > rhs.GetLutNum())
        return false;
    if (GetEdgeNum() < rhs.GetEdgeNum())
        return true;
    if (GetEdgeNum() > rhs.GetEdgeNum())
        return false;
    return false;
}

void
Solution::Remove(uint id)
{
    assert(_with_cover && _cuts[id]);
    Cut *cut = _cuts[id];
    _cuts[id] = nullptr;
    --_num_lut[cut->size];
    --_sum_lut;
    _sum_edge -= cut->size;
    delete cut;
}

void 
Solution::Add(uint id, Cut *cut)
{
    assert(_cuts[id] == nullptr);
    _cuts[id] = new Cut(*cut);
    ++_num_lut[cut->size];
    ++_sum_lut;
    _sum_edge += cut->size;
}

Time
Solution::GetDelayNum() const
{
    if (_max_arr)
        return _max_arr;

    std::vector<int> arr_time(_mapper->GetNodeNum(), 0);
    for (int i = 1; i != _mapper->GetNodeNum(); ++i)
    {
        if (Cut *cut = GetSol(i); cut)
        {
            for (int m = 0; m != cut->size; ++m)
                arr_time[i] = std::max(arr_time[cut->leaves[m]], arr_time[i]);
            arr_time[i] += _mapper->GetLutDelayCost(cut->size);
        }
    }

    for (Node *po : _mapper->_prim_outputs)
    {
        if (po->GetFanin0())
            _max_arr = std::max(_max_arr, (uint)arr_time[po->GetFanin0Id()]);
    }

    return _max_arr;
}

void
FoxMap::Initialize()
{
    _prim_inputs.reserve(Abc_NtkPiNum(_pAig));
    _prim_outputs.reserve(Abc_NtkPoNum(_pAig));

    // creat mapping graph
    _num_nodes = _pAig->vObjs->nSize + 1;

    _num_refs.resize(_num_nodes);

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
        
            _num_refs[i] = Abc_ObjFanoutNum(pObj);
        }
    }

    Abc_NtkCleanCopy(_pAig);

    // initialize default mapping settings
    _prune.SetMode(Prune::PruneMode::UL);
}

void
FoxMap::UpdateMapping(Solution *new_mapping)
{
    if (!_best_mapping)
        _best_mapping = new_mapping;
    else if (!_best_mapping->HasCover() || (*new_mapping) < (*_best_mapping))
    {
        delete _best_mapping;
        _best_mapping = new_mapping;
    }
    else
        delete new_mapping;
}

void
FoxMap::Print()
{
    printf("# Pi %ld, Po %ld, And %ld --> All %d\n", NumPi(), NumPo(), NumAnd(), _num_nodes);
    for (int i = 0; i != _num_nodes; ++i)
    {
        printf("## node %d\n", i);
        GetNode(i)->Print();
    }
}

Time
FoxMap::GetGlobalRequired()
{
    if (!_map_param->TimingDriven())
        return kMaxTime;

    if (!_max_po_arr_time)
    {
        for (Node *po : _prim_outputs)
        {
            if (po->GetFanin0())
                _max_po_arr_time = std::max(po->GetFanin0()->GetArr(), _max_po_arr_time);
        }
    }
    assert(_max_po_arr_time);

    if (_map_param->required == 0)
        return _max_po_arr_time;
    else if (_max_po_arr_time < _map_param->required)
        return _map_param->required;
    else
    {
        printf("foxmap: required time %ld cannot met\n", _map_param->required);
        return _max_po_arr_time;
    }
}

void
FoxMap::ReferenceBestCuts()
{
    _map_num_lut   = 0;
    _map_num_edge  = 0;
    _map_num_level = 0;

    for (int i = 1; i != _num_nodes; ++i)
    {
        Node *node = GetNode(i);
        node->GetRefNum() = 0;
        node->SetRequired(kMaxTime);
    }

    // reference the po drivers
    for (Node *po : _prim_outputs)
    {
        if (po->GetFanin0())
            ++po->GetFanin0()->GetRefNum();
    }

    // reference from PO to PIs
    if (_algo == Algo::Praetor)
    {
        auto recompute_cost = [this](Cut *cut) -> void
        {
            cut->area = GetLutAreaCost(cut->size);
            cut->edge = GetLutEdgeCost(cut->size);
            for (int i = 0; i != cut->size; ++i)
            {
                if (Node *leaf = GetNode(cut->leaves[i]); leaf->IsAnd() && leaf->GetRefNum() == 0)
                {
                    cut->area += leaf->GetArea();
                    cut->edge += leaf->GetEdge();
                }
            }
        };

        auto reorder_cuts = [recompute_cost, this](Node *node) -> Cut *
        {
            Time required = node->GetRequired();
            int best = -1;
            for (int i = 0; i != node->GetCutNum() - 1; ++i)
            {
                Cut *cut = node->GetCut(i);
                if (cut->arr > required)
                    continue;
                recompute_cost(cut);
                if (best == -1 || _cut_rank_enu_fn(cut, node->GetCut(best), 0.001) == 1)
                    best = i;
            }
            if (best == -1)
                return nullptr;
            else
                return node->GetCut(best);
        };

        // reorder the cuts
        for (int i = _num_nodes - 1; i != 1; --i)
        {
            if (Node *node = GetNode(i); node->IsAnd() && node->GetRefNum())
            {
                Cut *cut = reorder_cuts(node);
                if (cut)
                    *node->GetBestCut() = *cut;
                else
                    cut = node->GetBestCut();
                for (int m = 0; m != cut->size; ++m)
                    ++GetNode(cut->leaves[m])->GetRefNum();
        
                _map_num_lut  += GetLutAreaCost(cut->size);
                _map_num_edge += GetLutEdgeCost(cut->size);
        
                assert(cut->arr <= node->GetRequired());
            }
        }
    }
    else
    {
        for (int i = _num_nodes - 1; i != 1; --i)
        {
            if (Node *node = GetNode(i); node->IsAnd() && node->GetRefNum())
            {
                Cut *cut = node->GetBestCut();
                for (int m = 0; m != cut->size; ++m)
                    ++GetNode(cut->leaves[m])->GetRefNum();
        
                _map_num_lut  += GetLutAreaCost(cut->size);
                _map_num_edge += GetLutEdgeCost(cut->size);
        
                assert(cut->arr <= node->GetRequired());
            }
        }
    }

    // compute mapping level
    std::vector<uint> node_level(_num_nodes, 0);
    for (int i = 1; i != _num_nodes; ++i)
    {
        uint &level = node_level[i];
        if (Node *node = GetNode(i); node->IsAnd() && node->GetRefNum())
        {
            Cut *cut = node->GetBestCut();
            for (int m = 0; m != cut->size; ++m)
                level = std::max(level, node_level[cut->leaves[m]]);
            level += GetLutDelayCost(cut->size);
        }
        _map_num_level = std::max(_map_num_level, level);
    }

    _first_pass = false;
}

Solution *
FoxMap::CreateSolFromCurrMap()
{
    Solution *mapping = new Solution(this, _num_nodes);
    for (int i = 1; i != _num_nodes; ++i)
    {
        Node *node = GetNode(i);
        mapping->GetRefCount(i) = node->GetRefNum();
        if (node->IsAnd() && node->GetRefNum())
        {
            mapping->Add(i, node->GetBestCut());

        }
    }
    return mapping;
}

void
FoxMap::ComputeRequiredTime()
{
    // reference the best cuts to form mapping
    ReferenceBestCuts();

    Time required = GetGlobalRequired();
    if (required == kMaxTime)
        return;

    for (Node *po : _prim_outputs)
    {
        if (Node *fanin = po->GetFanin0(); fanin && fanin->IsAnd())
            fanin->SetRequired(required);
    }

    for (int i = _num_nodes; i != 0; --i)
    {
        Node *node = GetNode(i);
        if (!node->GetRefNum() || !node->IsAnd())
            continue;
        Cut *cut = node->GetBestCut();
        Time req = node->GetRequired() - GetLutDelayCost(cut->size);
        for (int i = 0; i != cut->size; ++i)
        {
            Node *leaf = GetNode(cut->leaves[i]);
            leaf->SetRequired(std::min(leaf->GetRequired(), req));
        }
    }
}

void
FoxMap::PerformGeneralMapping(Algo algo, RankFn fn)
{
    auto mapping_start = clock();

    // set the algorithm and funcs used for cut cost computation
    _algo = algo;
    _cut_rank_enu_fn = fn;

    // enumerate cuts
    for (int i = 1; i != _num_nodes; ++i)
    {
        Node *node = GetNode(i);
        if (node->IsPi() || node->IsAnd())
            node->CutEnum(this);
    }

    // compute and propagate required times
    ComputeRequiredTime();

    auto mapping_end = clock();

    if (_map_param->verbose)
    {
        const char *stage = nullptr;
        if (algo == Algo::Praetor)
            stage = "Pt";
        else if (_premap)
            stage = "Pm";
        else if (algo == Algo::Flow)
            stage = "Fl";
        else if (algo == Algo::Exact)
            stage = "Ex";
        else
            stage = "Ef";
        PrintMapping(stage, (mapping_end - mapping_start) / (float)CLOCKS_PER_SEC);
    }
}

void
FoxMap::PerformCutExpandsion(int lut_size)
{   
    auto compute_cut_cost = [](std::vector<int> &nodes) -> int
    {
        int cost = 0;
        for (int idx : nodes)
            if (GetNode(idx)->GetRefNum() == 0)
                ++cost;
        return cost;
    };

    for (int i = 1; i != _num_nodes; ++i)
    {
        Node *node = GetNode(i);
        if (!node->IsAnd() || !node->GetRefNum())
            continue;
        std::vector<int> front, front_old, visited;
        Cut *cut = node->GetBestCut();
        Time old_arr = cut->arr;
        cut->RipMFFC(this);
        Area old_area = cut->RefMFFC(this);
        // mark the cone nodes
        for (int m = 0; m != cut->size; ++m)
        {
            int leaf = cut->leaves[m];
            front.push_back(leaf);
            front_old.push_back(leaf);
            visited.push_back(leaf);
            GetNode(leaf)->SetMark(1);
        }
        cut->MarkCone(node, visited);

        cut->RipMFFC(this);

        int cost_before = compute_cut_cost(front);

    }
}

Abc_Ntk_t *
FoxMap::GenMappedNetwork(Solution *final_mapping)
{
    Abc_Ntk_t *mapped = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    mapped->pName = Extra_UtilStrsav(_pAig->pName);
    mapped->pSpec = Extra_UtilStrsav(_pAig->pSpec);

    int i;
    Abc_Obj_t *pNode;

    // create ports
    Abc_NtkForEachPi(_pAig, pNode, i)
    {
        Abc_Obj_t *pPi = Abc_NtkCreatePi(mapped);
        Abc_ObjAssignName(pPi, Abc_ObjName(pNode), nullptr);
        pNode->pCopy = pPi;
    }

    Abc_NtkForEachPo(_pAig, pNode, i)
    {
        Abc_Obj_t *pPo = Abc_NtkCreatePo(mapped);
        Abc_ObjAssignName(pPo, Abc_ObjName(pNode), nullptr);
    }

    // hook the const
    Abc_Obj_t *pConst1 = Abc_NtkCreateObj(mapped, ABC_OBJ_NODE);
    pConst1->pData = Abc_SopCreateConst1((Mem_Flex_t *)mapped->pManFunc);
    Abc_NtkObj(_pAig, 0)->pCopy = pConst1;

    // create LUT nodes
    for (int i = 1; i != _num_nodes; ++i)
    {
        if (!GetNode(i)->IsAnd() || final_mapping->GetRefCount(i) == 0)
            continue;
        Cut *cut = final_mapping->GetSol(i);
        Abc_Obj_t *pLut = Abc_NtkCreateObj(mapped, ABC_OBJ_NODE);
        if (word truth = cut->truth; truth == 0ul || truth == ~0ul)
        {
            Abc_ObjAddFanin(pLut, pConst1);
            if (truth == 0ul)
                pLut->pData = Abc_SopCreateBuf((Mem_Flex_t *)mapped->pManFunc);
            else
                pLut->pData = Abc_SopCreateInv((Mem_Flex_t *)mapped->pManFunc);
        }
        else
        {
            for (int m = 0; m != cut->size; ++m)
                Abc_ObjAddFanin(pLut, Abc_NtkObj(_pAig, cut->leaves[m])->pCopy);
            pLut->pData = Abc_SopRegister((Mem_Flex_t*)mapped->pManFunc,
                Abc_SopCreateFromTruth((Mem_Flex_t *)mapped->pManFunc, cut->size, (unsigned *)&truth));
        }
        Abc_NtkObj(_pAig, i)->pCopy = pLut;
    }

    Abc_NtkForEachPo(mapped, pNode, i)
    {
        Abc_Obj_t *pOldPo = Abc_NtkPo(_pAig, i);
        Abc_Obj_t *pFanin = Abc_ObjFanin0(pOldPo)->pCopy;
        if (pOldPo->fCompl0)
        {
            if (Abc_ObjFanoutNum(Abc_ObjFanin0(pOldPo)) || Abc_ObjFanin0(pOldPo)->Type == ABC_OBJ_PI)
            {
                Abc_Obj_t *pInv = Abc_NtkCreateNode(mapped);
                pInv->pData = Abc_SopCreateInv((Mem_Flex_t *)mapped->pManFunc);
                Abc_ObjAddFanin(pInv, pFanin);
                Abc_ObjAddFanin(pNode, pInv);
            }
            else
            {
                Abc_SopComplement((char *)pFanin->pData);
                Abc_ObjAddFanin(pNode, pFanin);
            }
        }
        else
        {
            Abc_ObjAddFanin(pNode, pFanin);
        }
    }

    Abc_NtkCleanCopy(_pAig);

    return mapped;
}

Abc_Ntk_t *
FoxMap::MapToLut()
{
    Initialize();

    // report the graph info
    if (_map_param->verbose)
        printf("mapping graph -- Pi %ld, Po %ld, And %ld\n", NumPi(), NumPo(), NumAnd());

    // set up LUT library
    SetupLib();

    if (_map_param->praetor_premap)
    {
        _premap = 1;
        _prune.SetMode(Prune::PruneMode::IDL);

        RankFn fn;
        if (_map_param->TimingDriven())
            fn = RankFnSet::CmpCutArrSizeAreaEdge;
        else if (_map_param->AreaDriven())
            fn = RankFnSet::CmpCutAreaEdge;
        else
            fn = RankFnSet::CmpCutEdgeArea;
        PerformGeneralMapping(Algo::Praetor, fn);

        _premap = 0;
        _prune.SetMode(Prune::PruneMode::UL);
    }

    if (_map_param->TimingDriven())
    {
        _premap = 1;
        std::vector<RankFn> rank_fn_set = {
            RankFnSet::CmpCutArrSizeAreaEdge,
            RankFnSet::CmpCutArrAreaEdge,
            RankFnSet::CmpCutAreaEdge
        };
        for (int i = 0; i != 3; ++i)
        {
            if (i)
            {
                for (int i = 1; i != _num_nodes; ++i)
                    GetNode(i)->GetRefNum() = _num_refs[i];
            }
            PerformGeneralMapping(Algo::Flow, rank_fn_set[i]);
        }

        _premap = 0;
    }

    RankFn fn = _map_param->tar == OptTarget::Area ? RankFnSet::CmpCutAreaEdge : RankFnSet::CmpCutEdgeArea;

    for (int i = 0; i != _map_param->flow_pass_num; ++i)
        PerformGeneralMapping(Algo::Flow, fn);

    for (int i = 0; i != _map_param->exact_pass_num; ++i)
        PerformGeneralMapping(Algo::Exact, fn);

    // mapping solution
    return GenMappedNetwork(CreateSolFromCurrMap());
}

void
FoxMap::LutCostLib::SyncUserLib()
{
    If_LibLut_t *lib = (If_LibLut_t *)Abc_FrameReadLibLut();
    if (!lib)
        return;
    printf("LUT area cost updated from lib ...\n");
    for (int i = 2; i != kMaxLutSize + 1; ++i)
        area_cost[0][i] = lib->pLutAreas[i];
}

} // namespace foxmap

Abc_Ntk_t *
PerformFoxMap(Abc_Ntk_t *pAig, foxmap::Param *param)
{
    if (Abc_NtkGetChoiceNum(pAig))
    {
        Abc_Ntk_t *pNew = Abc_NtkStrash(pAig = Abc_NtkDup(pAig), 0, 1, 0);
        Abc_NtkDelete(pAig);
        pAig = pNew;
        printf("foxmap: choices in AIG are removed.\n");
    }

    foxmap::Node::_const_1 = nullptr;
    foxmap::FoxMap mapper(param, pAig);

    return mapper.MapToLut();
}

} // namespace fox
