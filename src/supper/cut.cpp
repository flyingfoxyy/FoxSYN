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

namespace fox::supper
{
int
RankFnSet::cmp_cut_arr_size_area_edge(Cut *lhs, Cut *rhs, float epsilon)
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
RankFnSet::cmp_cut_arr_area_edge(Cut *lhs, Cut *rhs, float epsilon)
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
RankFnSet::cmp_cut_arr_edge_area(Cut *lhs, Cut *rhs, float epsilon)
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
RankFnSet::cmp_cut_area_edge(Cut *lhs, Cut *rhs, float epsilon)
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
RankFnSet::cmp_cut_edge_area(Cut *lhs, Cut *rhs, float epsilon)
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
Cut::merge_cut(Cut *lhs, Cut *rhs, int lut_size)
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
Cut::compute_cost(Algo algo, Node *node, Cut *lhs, Cut *rhs)
{
    if (algo == Algo::Flow)
    {
        area = Cut::get_area_cost(this);
        edge = Cut::get_edge_cost(this);
        for (int i = 0; i != size; ++i)
        {
            Node *leaf = Node::get_node(leaves[i]);
            area += leaf->get_area() / leaf->get_est_ref_num();
            edge += leaf->get_edge() / leaf->get_est_ref_num();
        }
    }
    else if (algo == Algo::Exact)
    {
        MffcInfo info0, info1;
        ref_mffc(info0);
        rip_mffc(info1);
        assert(info0.get_area() == info1.get_area() && info0.get_edge() == info1.get_edge());
        ratio = info0.get_ratio();
        area = info0.get_area();
        edge = info0.get_edge();
    }
    else
    {
        area = Cut::get_area_cost(this);
        area += (lhs->area - Cut::get_area_cost(lhs)) / node->get_fanin0()->get_est_ref_num();
        area += (rhs->area - Cut::get_area_cost(rhs)) / node->get_fanin1()->get_est_ref_num();
        edge = Cut::get_edge_cost(this);
        edge += (lhs->edge - Cut::get_edge_cost(lhs)) / node->get_fanin0()->get_est_ref_num();
        edge += (rhs->edge - Cut::get_edge_cost(rhs)) / node->get_fanin1()->get_est_ref_num();
    }

    // compute arrival time
    arr = compute_arr_time();
}

Time
Cut::compute_arr_time() const
{
    Time max_arr = 0;
    for (int i = 0; i != size; ++i)
        max_arr = std::max(max_arr, Node::get_node(leaves[i])->get_arr());
    return max_arr + Cut::get_delay_cost(this);
}

Area
Cut::ref_mffc()
{
    Area area = Cut::get_area_cost(this);
    for (int i = 0; i != size; ++i)
    {
        Node *node = Node::get_node(leaves[i]);
        if (node->get_ref_num()++ > 0 || !node->is_and())
            continue;
        area += node->get_best_cut()->ref_mffc();
    }
    return area;
}

Edge
Cut::rip_mffc()
{
    Edge edge = Cut::get_edge_cost(this);
    for (int i = 0; i != size; ++i)
    {
        Node *node = Node::get_node(leaves[i]);
        assert(node->get_ref_num() > 0);
        if (--node->get_ref_num() > 0 || !node->is_and())
            continue;
        edge += node->get_best_cut()->rip_mffc();
    }
    return edge;
}

void
Cut::ref_mffc(MffcInfo &info)
{
    info.add_node(this);
    for (int i = 0; i != size; ++i)
    {
        Node *node = Node::get_node(leaves[i]);
        if (node->get_ref_num()++ > 0 || !node->is_and())
            continue;
        node->get_best_cut()->ref_mffc(info);
    }
}

void
Cut::rip_mffc(MffcInfo &info)
{
    info.add_node(this);
    for (int i = 0; i != size; ++i)
    {
        Node *node = Node::get_node(leaves[i]);
        assert(node->get_ref_num() > 0);
        if (--node->get_ref_num() > 0 || !node->is_and())
            continue;
        node->get_best_cut()->rip_mffc(info);
    }

}

std::pair<Area, Edge>
Cut::get_mffc_cost_info()
{
    std::pair<Area, Edge> cost{Cut::get_area_cost(this), Cut::get_edge_cost(this)};
    for (int i = 0; i != size; ++i)
    {
        Node *node = Node::get_node(leaves[i]);
        if (node->is_and() && node->get_ref_num() == 1)
        {
            auto leaf_cost = node->get_best_cut()->get_mffc_cost_info();
            cost.first  += leaf_cost.first;
            cost.second += leaf_cost.second;
        }
    }
    return cost;
}

void
Cut::mark_cone(Node *node, std::vector<int> &cone)
{
    if (node->get_mark())
        return;
    assert(node->is_and());
    mark_cone(node->get_fanin0(), cone);
    mark_cone(node->get_fanin1(), cone);
    cone.push_back(node->get_id());
    node->set_mark(1);
}

void
Cut::compute_truth(Cut *lhs, Cut *rhs, int compl0, int compl1)
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
Cut::compute_truth(Node *root)
{
    std::vector<Node *> cone_nodes;
    cone_nodes.reserve(100);

    assert(size <= 6);

    for (int i = 0; i != size; ++i)
    {
        Node *node = Node::get_node(leaves[i]);
        assert(node->get_ref_num());
        assert(node->get_mark() == 0);
        node->set_mark(1);
        Abc_TtElemInit2(&node->get_truth(), i);
        cone_nodes.push_back(node);
    }

    std::function<void(Node *)> compute_tt_rec = [&compute_tt_rec, &cone_nodes](Node *node) -> void
    {
        if (node->get_mark())
            return;
        node->set_mark(1);
        compute_tt_rec(node->get_fanin0());
        compute_tt_rec(node->get_fanin1());
        word tt0 = node->get_fanin0()->get_truth();
        word tt1 = node->get_fanin1()->get_truth();
        if (node->get_compl0())
            tt0 = ~tt0;
        if (node->get_compl1())
            tt1 = ~tt1;
        node->get_truth() = tt0 & tt1;
        cone_nodes.push_back(node);
    };

    compute_tt_rec(root);

    truth = root->get_truth();

    // clear the node mark and truth table
    for (Node *node : cone_nodes)
    {
        node->set_mark(0);
        node->get_truth() = 0;
    }
}

void
Cut::print()
{
    printf("{Arr %d, Area %.1f, Edge %.1f, cut-set {%4d, %4d, %4d, %4d, %4d, %4d}}\n",
        arr, area, edge, leaves[0], leaves[1], leaves[2], leaves[3], leaves[4], leaves[5]);
}

int 
CutSet::get(Cut *&cut_set, uint capacity)
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
CutSet::push(Cut *cut)
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
CutSet::reset()
{
    std::fill(_temp_cuts, _temp_cuts + _temp_used_num, Cut{});
    std::fill(_unified_list.begin(), _unified_list.end(), nullptr);
    _min_area = kMaxArea;
    _temp_used_num = 0;
    for (auto &&cut_set: _indexed_list)
        std::fill(cut_set.begin(), cut_set.end(), nullptr);
}


} // end namespace foxmap
