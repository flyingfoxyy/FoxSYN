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

namespace fox
{
namespace foxmap
{
thread_local Node        * Node::s_const_1      = nullptr;
thread_local LutCostLib  * Cut ::s_lut_cost_lib = nullptr;

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
            arr_time[i] += Cut::GetDelayCost(cut);
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

    // set LUT lib for Cut
    foxmap::Cut::s_lut_cost_lib = &_lut_lib;

    // creat mapping graph
    _num_nodes = _pAig->vObjs->nSize + 1;

    _num_refs.resize(_num_nodes);

    Node::s_const_1 = new Node[_num_nodes];

    for (int i = 0; i != _pAig->vObjs->nSize; ++i)
    {
        if (Abc_Obj_t *pObj = Abc_NtkObj(_pAig, i); pObj)
        {
            Node *node = Node::s_const_1 + i;
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
    else if ((*new_mapping) < (*_best_mapping))
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
        Node::GetNode(i)->Print();
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
        Node *node = Node::GetNode(i);
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
            cut->area = Cut::GetAreaCost(cut);
            cut->edge = Cut::GetEdgeCost(cut);
            for (int i = 0; i != cut->size; ++i)
            {
                if (Node *leaf = Node::GetNode(cut->leaves[i]); leaf->IsAnd() && leaf->GetRefNum() == 0)
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
            if (Node *node = Node::GetNode(i); node->IsAnd() && node->GetRefNum())
            {
                Cut *cut = reorder_cuts(node);
                if (cut)
                    *node->GetBestCut() = *cut;
                else
                    cut = node->GetBestCut();
                for (int m = 0; m != cut->size; ++m)
                    ++Node::GetNode(cut->leaves[m])->GetRefNum();
        
                _map_num_lut  += Cut::GetAreaCost(cut);
                _map_num_edge += Cut::GetEdgeCost(cut);
        
                assert(cut->arr <= node->GetRequired());
            }
        }
    }
    else
    {
        for (int i = _num_nodes - 1; i != 1; --i)
        {
            if (Node *node = Node::GetNode(i); node->IsAnd() && node->GetRefNum())
            {
                Cut *cut = node->GetBestCut();
                for (int m = 0; m != cut->size; ++m)
                    ++Node::GetNode(cut->leaves[m])->GetRefNum();
        
                _map_num_lut  += Cut::GetAreaCost(cut);
                _map_num_edge += Cut::GetEdgeCost(cut);
        
                assert(cut->arr <= node->GetRequired());
            }
        }
    }

    // compute mapping level
    std::vector<uint> node_level(_num_nodes, 0);
    for (int i = 1; i != _num_nodes; ++i)
    {
        uint &level = node_level[i];
        if (Node *node = Node::GetNode(i); node->IsAnd() && node->GetRefNum())
        {
            Cut *cut = node->GetBestCut();
            for (int m = 0; m != cut->size; ++m)
                level = std::max(level, node_level[cut->leaves[m]]);
            level += Cut::GetDelayCost(cut);
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
        Node *node = Node::GetNode(i);
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
        Node *node = Node::GetNode(i);
        if (!node->GetRefNum() || !node->IsAnd())
            continue;
        Cut *cut = node->GetBestCut();
        Time req = node->GetRequired() - Cut::GetDelayCost(cut);
        for (int i = 0; i != cut->size; ++i)
        {
            Node *leaf = Node::GetNode(cut->leaves[i]);
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
        Node *node = Node::GetNode(i);
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

    // performa cut expandsion to reduce LUT number
    if (_map_param->expand_cut)
        PerformCutExpansion(6);
}

bool
FoxMap::NodeFaninCompact0(Node *node, std::vector<int> &front, std::vector<int> &visited)
{
    auto GetFaninCost = [&](Node *n) -> int
    {
        int counter = 0;
        if (n->GetRefNum() == 0)
            --counter;
        if (!n->GetFanin0()->GetMark() && n->GetFanin0()->GetRefNum() == 0)
            ++counter;
        if (!n->GetFanin1()->GetMark() && n->GetFanin1()->GetRefNum() == 0)
            ++counter;
        return counter;
    };

    auto FaninUpdate = [&](Node *n) -> void
    {
        // remove n from front
        std::vector<int> temp;
        for (int id : front)
            if (id != n->GetId())
                temp.push_back(id);
        std::swap(front, temp);
        Node *fanin = n->GetFanin0();
        if (fanin->GetMark() == 0)
        {
            front.push_back(fanin->GetId());
            visited.push_back(fanin->GetId());
            fanin->SetMark(1);
        }
        
        fanin = n->GetFanin1();
        if (fanin->GetMark() == 0)
        {
            front.push_back(fanin->GetId());
            visited.push_back(fanin->GetId());
            fanin->SetMark(1);
        }
    };

    for (int id : front)
    {
        Node *node = Node::GetNode(id);
        if (node->IsPi())
            continue;
        if (!node->GetFanin0()->GetMark() && !node->GetFanin1()->GetMark())
            continue;
        if (GetFaninCost(node) <= 0)
        {
            FaninUpdate(node);
            return 1;
        }
    }
    return 0;
}

bool
FoxMap::NodeFaninCompact1(Node *node, std::vector<int> &front, std::vector<int> &visited)
{
    auto GetFaninCost = [&](Node *n) -> int
    {
        int counter = 0;
        if (n->GetRefNum() == 0)
            --counter;
        if (!n->GetFanin0()->GetMark() && n->GetFanin0()->GetRefNum() == 0)
            ++counter;
        if (!n->GetFanin1()->GetMark() && n->GetFanin1()->GetRefNum() == 0)
            ++counter;
        return counter;
    };

    auto FaninUpdate = [&](Node *n) -> void
    {
        /// remove n from front
        std::vector<int> temp;
        for (int id : front)
            if (id != n->GetId())
                temp.push_back(id);
        std::swap(front, temp);
        // update
        Node *fanin = n->GetFanin0();
        if (fanin->GetMark() == 0)
        {
            front.push_back(fanin->GetId());
            visited.push_back(fanin->GetId());
            fanin->SetMark(1);
        }
        
        fanin = n->GetFanin1();
        if (fanin->GetMark() == 0)
        {
            front.push_back(fanin->GetId());
            visited.push_back(fanin->GetId());
            fanin->SetMark(1);
        }
    };

    for (int id : front)
    {
        Node *node = Node::GetNode(id);
        if (node->IsPi())
            continue;
        if (GetFaninCost(node) < 0)
        {
            FaninUpdate(node);
            return 1;
        }
    }
    return 0;
}

void
FoxMap::PerformCutExpansion(int lut_size)
{
    auto start = clock();

    auto compute_cut_cost = [](std::vector<int> &nodes) -> int
    {
        int cost = 0;
        for (int idx : nodes)
            if (Node::GetNode(idx)->GetRefNum() == 0)
                ++cost;
        return cost;
    };

    auto node_update = [this](Node *node, std::vector<int> &front)
    {
        Cut *cut = node->GetBestCut();
        cut->RipMFFC();
        cut->size = front.size();
        assert(cut->size <= 6);
        cut->sign = 0;
        for (int i = 0; i != front.size(); ++i)
        {
            cut->sign |= GetSign(cut->leaves[i]);
            cut->leaves[i] = front[i];
        }
        std::sort(cut->leaves, cut->leaves + cut->size);
        cut->RefMFFC();
        assert(cut->IsValid());
    };

    for (int i = 1; i != _num_nodes; ++i)
    {
        Node *node = Node::GetNode(i);
        if (!node->IsAnd() || !node->GetRefNum())
            continue;
        std::vector<int> front, front_old, visited;
        Cut *cut = node->GetBestCut();
        Time old_arr = cut->arr;
        cut->RipMFFC();
        Area old_area = cut->RefMFFC();
        // mark the cone nodes
        // improve prepare
        for (int m = 0; m != cut->size; ++m)
        {
            int leaf = cut->leaves[m];
            front.push_back(leaf);
            front_old.push_back(leaf);
            visited.push_back(leaf);
            Node::GetNode(leaf)->SetMark(1);
        }
        // mark the nodes in the cone
        cut->MarkCone(node, visited);
        // end prepare

        // rip-up the cone
        cut->RipMFFC();

        // compute initial cost
        int cost_before = compute_cut_cost(front);

        // If_ManImproveNodeFaninCompact
        while (true)
        {
            bool result = false;
            if (NodeFaninCompact0(node, front, visited) )
                result = true;
            else if (front.size() < 6 && NodeFaninCompact1(node, front, visited))
                result = true;
            else
                result = false;

            assert(front.size() <= 6);

            if (!result)
                break;
        }

        int cost_after = compute_cut_cost(front);
        cut->RefMFFC();
        assert(cost_before >= cost_after);
        
        for (int id : visited)
            Node::GetNode(id)->SetMark(0);

        node_update(node, front);

        cut->arr = cut->ComputeArrTime();
        cut->RipMFFC();
        Area new_area = cut->RefMFFC();

        // if there is no gain or required is not met
        // recall the previous one
        if (new_area > old_area || cut->arr > node->GetRequired())
        {
            node_update(node, front_old);
            cut->RipMFFC();
            new_area = cut->RefMFFC();
            assert(new_area == old_area);
            cut->arr = old_arr;
        }
        else
        {
            cut->ComputeTruth(node);
        }
        assert(cut->size <= 6);
    }

    ComputeRequiredTime();

    auto end = clock();

    if (_map_param->verbose)
        PrintMapping("EP", (end - start) / (float)CLOCKS_PER_SEC);
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
        if (!Node::GetNode(i)->IsAnd() || final_mapping->GetRefCount(i) == 0)
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
                    Node::GetNode(i)->GetRefNum() = _num_refs[i];
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

    foxmap::Node::s_const_1      = nullptr;
    foxmap::Cut ::s_lut_cost_lib = nullptr;
    foxmap::FoxMap mapper(param, pAig);

    return mapper.MapToLut();
}

} // namespace fox
