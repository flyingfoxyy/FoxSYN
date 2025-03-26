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

#include "misc/util/utilTruth.h"

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
    Algo algo = mapper->GetParam()->curr_algo;
    if (algo == Algo::Praetor)
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
    else if (algo == Algo::Flow)
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
            if ( k < i )
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
        _type   = NodeType::PI;
        _fanin0 = kMaxId;
        _fanin1 = kMaxId;
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
    if (!IsPi() && !IsAnd())
        return;

    if (_cut_set)
    {
        assert(mapper->GetParam()->always_enum_cut);
        delete[] _cut_set;
        _cut_set = nullptr;
    }

    if (IsPi())
    {
        _num_cuts = 1;
        _cut_set = new Cut[1];
        Cut *trivial_cut = GetTrivialCut();
        trivial_cut->leaves[0] = GetId();
        trivial_cut->sign = GetSign(GetId());
        if (mapper->GetParam()->curr_algo == Algo::Praetor)
            trivial_cut->area = mapper->GetLutCost(1);
        return;
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
            cut->sign = lhs->sign | rhs->sign;
            cut->ComputeCost(lhs, rhs, est_ref0, est_ref1, mapper);
            if (prune.Push(cut))
                cut->ComputeTruth(lhs, rhs, _compl0, _compl1);
        }
    }

    // pop the cuts
    _num_cuts = prune.Pop(_cut_set);

    // create trivial cut
    Cut *trival_cut = GetTrivialCut();
    trival_cut->leaves[0] = GetId();
    trival_cut->sign = GetSign(GetId());
    if (mapper->GetParam()->curr_algo == Algo::Praetor)
        trival_cut->area = GetCut(0)->area + mapper->GetLutCost(1);
    else
        trival_cut->area = GetCut(0)->area;
    trival_cut->edge = GetCut(0)->edge;
}

int 
Prune::Pop(Cut *&cut_set)
{
    std::vector<Cut *> cuts;
    for (auto it = _unified_list.begin(); it != _unified_list.end(); ++it)
    {
        if (*it)
            cuts.push_back(*it);
        else
            break;
    }
    cut_set = new Cut[cuts.size() + 1];  // extra one for trivial cut

    int cut_num = 0;
    for (Cut *cut : cuts)
        cut_set[cut_num++] = *cut;

    std::fill(_unified_list.begin(), _unified_list.end(), nullptr);

    return cuts.size() + 1;
}

bool
Prune::Push(Cut *cut)
{
    for (int i = 0; i != _unified_list.size(); ++i)
    {
        // rank to the last one
        if (!_unified_list[i])
        {
            if (i == _unified_list.size() - 1)
            {
                assert(_unified_used_num == _unified_list.size() - 1);
                return false;
            }
            _unified_list[i] = cut;
            _unified_used_num++;
            assert(_unified_list[i - 1]);
            return true;
        }
        // compare with current cut
        const int cmp_res = CmpCutAreaEdge(cut, _unified_list[i], _epsilon);
        // win
        if (cmp_res == 1)
        {
            for (int m = std::min(_unified_list.size() - 1, (std::size_t)_unified_used_num); m != i; --m)
                _unified_list[m] = _unified_list[m - 1];
            _unified_list[i] = cut;
            if (_unified_list.back())
                _unified_list.back() = nullptr;
            else
                _unified_used_num++;
            return true;
        }
        // same quality, only keep one
        else if (cmp_res == 0)
            return false;
    }

    assert(0 && "should arrival here");
    return true;
}

Solution::Solution(FoxMap *map) : _mapper(map)
{
    _cuts.resize(_mapper->_num_nodes);
    _ref_counter.resize(_mapper->_num_nodes);
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
        }
    }

    Abc_NtkCleanCopy(_pAig);
}

Cut *
FoxMap::SelectBestCut(Solution *curr_map, Cut *cut_set, int num, Algo algo)
{
    return cut_set;
}

Solution *
FoxMap::PerformMapping(Algo algo)
{
    auto mapping_start = clock();
    // set the algorithm used for cut cost computation
    _map_param->curr_algo = algo;

    // compute the reference count
    if (_first_pass)
    {
        int i = 0;
        for (float &est_ref : _est_refs)
            est_ref = _num_refs[i++];
    }
    else
    {
        if (_map_param->ref_est_way)
        {
            int i = 0;
            for (float &est_ref : _est_refs)
                est_ref = (_map_param->alpha * est_ref + _num_refs[i++]) / (1.0 + _map_param->alpha);
        }
    }

    Area estimated = 0;

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

    if (_map_param->verbose)
    {
        for (Node *node : _prim_outputs)
            estimated += node->GetFanin0()->GetArea() / GetEstRef(node->GetFanin0Id());
    }

    Solution *mapping = new Solution(this);
    
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
        mapping->Add(i, *best);
    }

    if (_first_pass)
        _first_pass = false;

    auto mapping_end = clock();

    if (_map_param->verbose)
    {
        const char *algo_str = algo == Algo::Praetor ? "EF" : (algo == Algo::Flow ? "FL" : "EX");
        printf("%s  Est = %4.1f Area = %6d  Edge = %6d\n", algo_str, estimated, mapping->GetLutNum(), mapping->GetEdgeNum());
    }

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

    // create LUT nodes
    for (int i = 1; i != _num_nodes; ++i)
    {
        if (!GetNode(i)->IsAnd() || final_mapping->GetRefCount(i) == 0)
            continue;
        Cut *cut = final_mapping->GetSol(i);
        Abc_Obj_t *pLut = Abc_NtkCreateObj(mapped, ABC_OBJ_NODE);
        if (word truth = cut->truth; truth == 0ul || truth == ~0ul)
        {
            Abc_Obj_t *pConst1 = Abc_NtkObj(_pAig, 0)->pCopy;
            if (pConst1 == nullptr)
            {
                pConst1 = Abc_NtkCreateObj(mapped, ABC_OBJ_NODE);
                pConst1->pData = Abc_SopCreateConst1((Mem_Flex_t *)mapped->pManFunc);
                Abc_NtkObj(_pAig, 0)->pCopy = pConst1;
            }
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

    // the mapping, which is always the best one
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
    // for (int i = 0; i != _map_param->praetor_pass_num; ++i)
    // {
    //     Solution *mapping = PerformMapping(Algo::Praetor);
    //     update_mapping(mapping);
    // }

    for (int i = 0; i != _map_param->flow_pass_num; ++i)
    {
        Solution *mapping = PerformMapping(Algo::Flow);
        update_mapping(mapping);
        int k = 0;
        for (float &est_ref : _est_refs)
            est_ref = static_cast<float>(curr_mapping->GetRefCount(k++));
    }

    // mapping solution
    return GenMappedNetwork(curr_mapping);
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
