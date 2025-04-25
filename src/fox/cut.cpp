/*============================================================================\
|                                                                             |
| file:      cut.cpp                                                          |
| author:    Longfei                                                          |
| purpose:   implementation of cut                                            |
| version:   0.1                                                              |
| date:      2025-3-23                                                        |
\============================================================================*/

#include <algorithm>

#include "foxmap.hpp"

namespace fox::foxmap
{
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
    if (lhs->ratio + epsilon < rhs->ratio)
        return 1;
    if (lhs->ratio - epsilon > rhs->ratio)
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
    if (lhs->ratio + epsilon < rhs->ratio)
        return 1;
    if (lhs->ratio - epsilon > rhs->ratio)
        return -1;
    if (lhs->size < rhs->size)
        return 1;
    if (lhs->size > rhs->size)
        return -1;
    return 0;
}

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
Cut::ComputeCost(Algo algo, Node *node, Cut *lhs, Cut *rhs)
{
    if (algo == Algo::Flow)
    {
        area = Cut::GetAreaCost(this);
        edge = Cut::GetEdgeCost(this);
        for (int i = 0; i != size; ++i)
        {
            Node *leaf = Node::GetNode(leaves[i]);
            area += leaf->GetArea() / leaf->GetEstRefNum();
            edge += leaf->GetEdge() / leaf->GetEstRefNum();
        }
    }
    else if (algo == Algo::Exact)
    {
        MffcInfo info0, info1;
        RefMFFC(info0);
        RipMFFC(info1);
        assert(info0.GetArea() == info1.GetArea() && info0.GetEdge() == info1.GetEdge());
        ratio = info0.GetRatio();
        area = info0.GetArea();
        edge = info0.GetEdge();
    }
    else
    {
        area = Cut::GetAreaCost(this);
        area += (lhs->area - Cut::GetAreaCost(lhs)) / node->GetFanin0()->GetEstRefNum();
        area += (rhs->area - Cut::GetAreaCost(rhs)) / node->GetFanin1()->GetEstRefNum();
        edge = Cut::GetEdgeCost(this);
        edge += (lhs->edge - Cut::GetEdgeCost(lhs)) / node->GetFanin0()->GetEstRefNum();
        edge += (rhs->edge - Cut::GetEdgeCost(rhs)) / node->GetFanin1()->GetEstRefNum();
    }

    // compute arrival time
    arr = ComputeArrTime();
}

Time
Cut::ComputeArrTime() const
{
    Time max_arr = 0;
    for (int i = 0; i != size; ++i)
        max_arr = std::max(max_arr, Node::GetNode(leaves[i])->GetArr());
    return max_arr + Cut::GetDelayCost(this);
}

Area
Cut::RefMFFC()
{
    Area area = Cut::GetAreaCost(this);
    for (int i = 0; i != size; ++i)
    {
        Node *node = Node::GetNode(leaves[i]);
        if (node->GetRefNum()++ > 0 || !node->IsAnd())
            continue;
        area += node->GetBestCut()->RefMFFC();
    }
    return area;
}

Edge
Cut::RipMFFC()
{
    Edge edge = Cut::GetEdgeCost(this);
    for (int i = 0; i != size; ++i)
    {
        Node *node = Node::GetNode(leaves[i]);
        assert(node->GetRefNum() > 0);
        if (--node->GetRefNum() > 0 || !node->IsAnd())
            continue;
        edge += node->GetBestCut()->RipMFFC();
    }
    return edge;
}

void
Cut::RefMFFC(MffcInfo &info)
{
    info.AddNode(this);
    for (int i = 0; i != size; ++i)
    {
        Node *node = Node::GetNode(leaves[i]);
        if (node->GetRefNum()++ > 0 || !node->IsAnd())
            continue;
        node->GetBestCut()->RefMFFC(info);
    }
}

void
Cut::RipMFFC(MffcInfo &info)
{
    info.AddNode(this);
    for (int i = 0; i != size; ++i)
    {
        Node *node = Node::GetNode(leaves[i]);
        assert(node->GetRefNum() > 0);
        if (--node->GetRefNum() > 0 || !node->IsAnd())
            continue;
        node->GetBestCut()->RipMFFC(info);
    }

}

std::pair<Area, Edge>
Cut::GetMFFCCostInfo()
{
    std::pair<Area, Edge> cost{Cut::GetAreaCost(this), Cut::GetEdgeCost(this)};
    for (int i = 0; i != size; ++i)
    {
        Node *node = Node::GetNode(leaves[i]);
        if (node->IsAnd() && node->GetRefNum() == 1)
        {
            auto leaf_cost = node->GetBestCut()->GetMFFCCostInfo();
            cost.first  += leaf_cost.first;
            cost.second += leaf_cost.second;
        }
    }
    return cost;
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
Cut::ComputeTruth(Node *root)
{
    std::vector<Node *> cone_nodes;
    cone_nodes.reserve(100);

    assert(size <= 6);

    for (int i = 0; i != size; ++i)
    {
        Node *node = Node::GetNode(leaves[i]);
        assert(node->GetRefNum());
        assert(node->GetMark() == 0);
        node->SetMark(1);
        Abc_TtElemInit2(&node->GetTruth(), i);
        cone_nodes.push_back(node);
    }

    std::function<void(Node *)> compute_tt_rec = [&compute_tt_rec, &cone_nodes](Node *node) -> void
    {
        if (node->GetMark())
            return;
        node->SetMark(1);
        compute_tt_rec(node->GetFanin0());
        compute_tt_rec(node->GetFanin1());
        word tt0 = node->GetFanin0()->GetTruth();
        word tt1 = node->GetFanin1()->GetTruth();
        if (node->GetCompl0())
            tt0 = ~tt0;
        if (node->GetCompl1())
            tt1 = ~tt1;
        node->GetTruth() = tt0 & tt1;
        cone_nodes.push_back(node);
    };

    compute_tt_rec(root);

    truth = root->GetTruth();

    // clear the node mark and truth table
    for (Node *node : cone_nodes)
    {
        node->SetMark(0);
        node->GetTruth() = 0;
    }
}

void
Cut::Print()
{
    printf("{Arr %d, Area %.1f, Edge %.1f, cut-set {%4d, %4d, %4d, %4d, %4d, %4d}}\n",
        arr, area, edge, leaves[0], leaves[1], leaves[2], leaves[3], leaves[4], leaves[5]);
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


} // end namespace foxmap
