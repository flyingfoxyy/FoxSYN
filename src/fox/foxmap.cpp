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

static int CmpCutAreaEdge(Cut *lhs, Cut *rhs, float epsilon)
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

static int CmpCutEdgeArea(Cut *lhs, Cut *rhs, float epsilon)
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
Cut::ComputeCost(Cut *lhs, Cut *rhs, float est_ref_l, float est_ref_r, FoxMap *mapper)
{
    Algo algo = mapper->GetAlgo();
    if (algo == Algo::Praetor)
    {
        area = mapper->GetLutAreaCost(size);
        area += (lhs->area - mapper->GetLutAreaCost(lhs->size)) / est_ref_l;
        area += (rhs->area - mapper->GetLutAreaCost(rhs->size)) / est_ref_r;
        edge = mapper->GetLutEdgeCost(size);
        edge += (lhs->edge - mapper->GetLutEdgeCost(lhs->size)) / est_ref_l;
        edge += (rhs->edge - mapper->GetLutEdgeCost(rhs->size)) / est_ref_r;
    }
    else if (algo == Algo::Flow)
    {
        area = mapper->GetLutAreaCost(size);
        edge = mapper->GetLutEdgeCost(size);
        for (int i = 0; i != size; ++i)
        {
            Node *leaf = mapper->GetNode(leaves[i]);
            area += leaf->GetArea() / mapper->GetEstRef(leaf->GetId());
            edge += leaf->GetEdge() / mapper->GetEstRef(leaf->GetId());
        }
    }
    else
    {

    }
    // compute arrival time

}

Area
Cut::RefMFFC(Solution *mapping, bool update)
{
    Area area = mapping->GetMapper()->GetLutAreaCost(size);
    for (int i = 0; i != size; ++i)
    {
        uint id = leaves[i];
        if (mapping->GetRefCount(id)++ > 0 || !FoxMap::GetNode(id)->IsAnd())
            continue;
        // with/without update, the best cut is always located on the first place
        Cut *best = FoxMap::GetNode(id)->GetBestCut();
        if (update)
            mapping->Add(id, best);
        area += best->RefMFFC(mapping, update);
    }
    return area;
}

Edge
Cut::RipMFFC(Solution *mapping, bool update)
{
    Edge edge = mapping->GetMapper()->GetLutEdgeCost(size);
    for (int i = 0; i != size; ++i)
    {
        uint id = leaves[i];
        if (--mapping->GetRefCount(id) > 0 || !FoxMap::GetNode(id)->IsAnd())
            continue;
        Cut *curr = update ? mapping->GetSol(id) : FoxMap::GetNode(id)->GetBestCut();
        edge += curr->RipMFFC(mapping, update);
        if (update)
            mapping->Remove(id);
    }
    return edge;
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

Node::Node(Abc_Obj_t *abc_node) : _num_cuts(0)
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
        break;
    case ABC_OBJ_PO:
        _type   = NodeType::PO;
        _fanin0 = Abc_ObjFaninId0(abc_node);
        _fanin1 = kMaxId;
        break;
    case ABC_OBJ_NODE:
        _type   = NodeType::And;
        _fanin0 = Abc_ObjFaninId0(abc_node);
        _fanin1 = Abc_ObjFaninId1(abc_node);
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
    if (IsPi())
    {
        GetTrivialCut()->area = mapper->GetAlgo() == Algo::Praetor ? mapper->GetLutAreaCost(1) : 0;
        GetTrivialCut()->edge = mapper->GetAlgo() == Algo::Praetor ? mapper->GetLutEdgeCost(1) : 0;
        return;
    }

    if (IsPo())
    {

    }

    Prune &prune = mapper->GetPrune();

    Node *fanin0 = GetFanin0();
    Node *fanin1 = GetFanin1();

    float est_ref0 = mapper->GetEstRef(_fanin0);
    float est_ref1 = mapper->GetEstRef(_fanin1);
    const int k = mapper->GetParam()->lut_size;

    // merge the cuts from fanins
    for (int i = 0; i != fanin0->GetCutNum(); ++i)
    {
        Cut *lhs = fanin0->GetCut(i);
        for (int m = 0; m != fanin1->GetCutNum(); ++m)
        {
            Cut *rhs = fanin1->GetCut(m);
            if (lhs->size + rhs->sign > k && fox::popcount(lhs->sign | rhs->sign) > k)
                continue;
            Cut *cut = prune.GetCandidate();
            // check cut is k-feasible or not
            if (!cut->MergeCut(lhs, rhs, k))
                continue;
            cut->ComputeCost(lhs, rhs, est_ref0, est_ref1, mapper);
            assert(cut->area > 0 && cut->area < kMaxArea);
            assert(cut->edge > 0 && cut->edge < kMaxArea);
            if (prune.Push(cut))
            {
                cut->ComputeTruth(lhs, rhs, _compl0, _compl1);
                cut->sign = lhs->sign | rhs->sign;
            }
        }
    }

    // pop the cuts
    _num_cuts = 1 + prune.Pop(_cut_set, _num_cuts);

    // create trivial cut
    Cut *trival_cut = GetTrivialCut();
    trival_cut->sign = GetSign(GetId());
    trival_cut->leaves[0] = GetId();
    trival_cut->edge = GetCut(0)->edge;
    trival_cut->area = GetCut(0)->area;
    if (mapper->GetAlgo() == Algo::Praetor)
    {
        trival_cut->area += mapper->GetLutAreaCost(1);
        trival_cut->edge += mapper->GetLutEdgeCost(1);
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
        const int cmp_res = CmpCutAreaEdge(cut, cut_list[i], _epsilon);
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
    assert(_cuts[id]);
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
        
            _num_refs[i] = Abc_ObjFanoutNum(pObj);
            _est_refs[i] = Abc_ObjFanoutNum(pObj);
        }
    }

    Abc_NtkCleanCopy(_pAig);
}

void
FoxMap::UpdateMapping(Solution *new_mapping)
{
    if (!_best_mapping)
        _best_mapping = new_mapping;
    else if ((*new_mapping) < (*_best_mapping))
    {
        delete _best_mapping;
        _best_mapping = new_mapping;
    }
    else
        delete new_mapping;
}

Cut *
FoxMap::SelectBestCut(Solution *curr_map, Cut *cut_set, int cut_num, Algo algo)
{
    if (algo == Algo::Flow)
        return cut_set;
    // for praetor, we compute cut cost according to realtime mapping
    auto compute_cost = [curr_map, this](Cut *cut)
    {
        cut->area = this->GetLutAreaCost(cut->size);
        cut->edge = this->GetLutEdgeCost(cut->size);
        for (int i = 0; i != cut->size; ++i)
        {
            uint leaf = cut->leaves[i];
            cut->area += curr_map->GetRefCount(leaf) ? 0 : GetNode(leaf)->GetArea() / GetEstRef(leaf);
            cut->edge += curr_map->GetRefCount(leaf) ? 0 : GetNode(leaf)->GetEdge() / GetEstRef(leaf);
        }
        cut->area /= cut->size;
    };
    Cut *winner = cut_set;
    compute_cost(winner);
    for (int i = 1; i != cut_num - 1; ++i)
    {
        compute_cost(cut_set + i);
        if (CmpCutAreaEdge(cut_set + i, winner, 0.001) == 1)
            winner = cut_set + i;
    }
    return winner;
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

void
FoxMap::ImproveMapping(Solution *mapping)
{
    auto start = clock();

    for (int i = 0; i != _num_nodes; ++i)
    {
        Node *node = GetNode(i);
        if (!node->IsAnd())
            continue;
        const bool referenced = mapping->GetRefCount(i);
        if (referenced)
        {
            mapping->GetSol(i)->RipMFFC(mapping, true);
            mapping->Remove(i);
        }
        // reorder the cuts for minimal MFFC size
        int best_idx = -1;
        for (int m = 0; m != node->GetCutNum() - 1; ++m)
        {
            Cut *cut = node->GetCut(m);
            cut->area = cut->RefMFFC(mapping);
            cut->edge = cut->RipMFFC(mapping);
            if (best_idx == -1 || CmpCutAreaEdge(cut, node->GetCut(best_idx), 0.1) == 1)
                best_idx = m;
        }
        // update
        node->SetBestCut(best_idx);
        if (referenced)
        {
            mapping->Add(i, node->GetBestCut());
            node->GetBestCut()->RefMFFC(mapping, true);
        }
    }

    if (_map_param->verbose)
        mapping->Print("EX", (clock() - start) / (float)CLOCKS_PER_SEC);
}

Solution *
FoxMap::PerformMapping(Algo algo)
{
    auto mapping_start = clock();

    // set the algorithm used for cut cost computation
    _algo = algo;

    // recall the best mapping to update reference
    // compute the estimated reference count
    if (!_first_pass)
    {
        int i = 0;
        for (float &est_ref : _est_refs)
            est_ref = std::max(1u, _best_mapping->GetRefCount(i++));
    }

    // enumerate cuts
    if (_map_param->always_enum_cut || _first_pass)
    {
        for (int i = 0; i != _num_nodes; ++i) {
            Node *node = GetNode(i);
            if (node->IsPi() || node->IsAnd())
                node->CutEnum(this);
        }
    }
    else
    {
        // update the cut cost
    }

    // if (_map_param->verbose)
    // {
    //     Area estimated = 0;
    //     for (Node *node : _prim_outputs)
    //     {
    //         Node *fanin = node->GetFanin0();
    //         if (fanin && fanin->IsAnd())
    //             estimated += fanin->GetArea() / GetEstRef(node->GetFanin0Id());
    //     }
    //     printf("-- Est = %.1f  \n", estimated);
    // }

    Solution *mapping = new Solution(this, this->_num_nodes);

    // perform cut selection
    for (Node *po : _prim_outputs)
    {
        if (Node *fanin = po->GetFanin0(); fanin && fanin->IsAnd())
            ++mapping->GetRefCount(fanin->GetId());
    }

    for (int i = _num_nodes - 1; i != -1; --i)
    {
        Node *node = GetNode(i);
        if (mapping->GetRefCount(i) == 0 || !node->IsAnd())
            continue;
        // find the best cut for current mapping
        Cut *best = SelectBestCut(mapping, node->GetCut(0), node->GetCutNum(), algo);
        for (int m = 0; m != best->size; ++m)
            ++mapping->GetRefCount(best->leaves[m]);
        mapping->Add(i, best);
    }

    if (_first_pass)
        _first_pass = false;

    auto mapping_end = clock();
    auto cpu_time = (mapping_end - mapping_start) / (float)CLOCKS_PER_SEC;

    if (_map_param->verbose)
        mapping->Print(algo == Algo::Praetor ? "EF" : (algo == Algo::Flow ? "FL" : "EX"), cpu_time);
    
    ImproveMapping(mapping);

    return mapping;
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

    _best_mapping = nullptr;

    // perform mapping pass with different heuristics
    for (int i = 0; i != _map_param->praetor_pass_num; ++i)
    {
        _prune.SetMode(Prune::PruneMode::IDLP);
        Solution *mapping = PerformMapping(Algo::Praetor);
        UpdateMapping(mapping);
    }

    for (int i = 0; i != _map_param->flow_pass_num; ++i)
    {
        _prune.SetMode(Prune::PruneMode::UL);
        Solution *mapping = PerformMapping(Algo::Flow);
        UpdateMapping(mapping);
    }

    // mapping solution
    return GenMappedNetwork(_best_mapping);
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
