/*============================================================================\
|                                                                             |
| file:      foxmap.cpp                                                       |
| author:    Longfei                                                          |
| purpose:   implementation of foxmap                                         |
| version:   0.1                                                              |
| date:      2025-3-23                                                        |
\============================================================================*/

#include <iostream>
#include <chrono>
#include <algorithm>
#include <utility>

#include "/home/longfei/taskflow/taskflow/taskflow.hpp"


#include "foxmap.hpp"

namespace fox
{
namespace supper
{
Node        * Node::s_const_1      = nullptr;
LutCostLib  * Cut ::s_lut_cost_lib = nullptr;

bool
Solution::operator<(const Solution& rhs)
{
    if (_mapper->get_param()->tar == OptTarget::Timing)
    {
        if (get_delay_num() < rhs.get_delay_num())
            return true;
        if (get_delay_num() > rhs.get_delay_num())
            return false;
    }
    if (get_lut_num() < rhs.get_lut_num())
        return true;
    if (get_lut_num() > rhs.get_lut_num())
        return false;
    if (get_edge_num() < rhs.get_edge_num())
        return true;
    if (get_edge_num() > rhs.get_edge_num())
        return false;
    return false;
}

void
Solution::remove(uint id)
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
Solution::add(uint id, Cut *cut)
{
    assert(_cuts[id] == nullptr);
    _cuts[id] = new Cut(*cut);
    ++_num_lut[cut->size];
    ++_sum_lut;
    _sum_edge += cut->size;
}

Time
Solution::get_delay_num() const
{
    if (_max_arr)
        return _max_arr;

    std::vector<int> arr_time(_mapper->get_node_num(), 0);
    for (int i = 1; i != _mapper->get_node_num(); ++i)
    {
        if (Cut *cut = get_sol(i); cut)
        {
            for (int m = 0; m != cut->size; ++m)
                arr_time[i] = std::max(arr_time[cut->leaves[m]], arr_time[i]);
            arr_time[i] += Cut::get_delay_cost(cut);
        }
    }

    for (Node *po : _mapper->_prim_outputs)
    {
        if (po->get_fanin0())
            _max_arr = std::max(_max_arr, (uint)arr_time[po->get_fanin0_id()]);
    }

    return _max_arr;
}

void
FoxMap::initialize()
{
    _prim_inputs.reserve(Abc_NtkPiNum(_pAig));
    _prim_outputs.reserve(Abc_NtkPoNum(_pAig));

    // set LUT lib for Cut
    supper::Cut::s_lut_cost_lib = &_lut_lib;

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

            if (node->is_pi())
                _prim_inputs.push_back(node);
            else if (node->is_po())
                _prim_outputs.push_back(node);

            _num_refs[i] = Abc_ObjFanoutNum(pObj);
        }
    }

    Abc_NtkCleanCopy(_pAig);

    // initialize default mapping settings
    _cut_set.set_mode(CutSet::PruneMode::UL);
}

void
FoxMap::update_mapping(Solution *new_mapping)
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
FoxMap::print()
{
    printf("# Pi %ld, Po %ld, And %ld --> All %d\n", num_pi(), num_po(), num_and(), _num_nodes);
    for (int i = 0; i != _num_nodes; ++i)
    {
        printf("## node %d\n", i);
        Node::get_node(i)->print();
    }
}

Time
FoxMap::get_global_required()
{
    if (!_map_param->timing_driven())
        return kMaxTime;

    if (!_max_po_arr_time)
    {
        for (Node *po : _prim_outputs)
        {
            if (po->get_fanin0())
                _max_po_arr_time = std::max(po->get_fanin0()->get_arr(), _max_po_arr_time);
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
FoxMap::reference_best_cuts()
{
    _map_num_lut   = 0;
    _map_num_edge  = 0;
    _map_num_level = 0;

    for (int i = 1; i != _num_nodes; ++i)
    {
        Node *node = Node::get_node(i);
        node->get_ref_num() = 0;
        node->set_required(kMaxTime);
    }

    // reference the po drivers
    for (Node *po : _prim_outputs)
    {
        if (po->get_fanin0())
            ++po->get_fanin0()->get_ref_num();
    }

    // reference from PO to PIs
    if (_algo == Algo::Praetor)
    {
        auto recompute_cost = [this](Cut *cut) -> void
        {
            cut->area = Cut::get_area_cost(cut);
            cut->edge = Cut::get_edge_cost(cut);
            for (int i = 0; i != cut->size; ++i)
            {
                if (Node *leaf = Node::get_node(cut->leaves[i]); leaf->is_and() && leaf->get_ref_num() == 0)
                {
                    cut->area += leaf->get_area();
                    cut->edge += leaf->get_edge();
                }
            }
        };

        auto reorder_cuts = [recompute_cost, this](Node *node) -> Cut *
        {
            Time required = node->get_required();
            int best = -1;
            for (int i = 0; i != node->get_cut_num() - 1; ++i)
            {
                Cut *cut = node->get_cut(i);
                if (cut->arr > required)
                    continue;
                recompute_cost(cut);
                if (best == -1 || _cut_rank_enu_fn(cut, node->get_cut(best), 0.001) == 1)
                    best = i;
            }
            if (best == -1)
                return nullptr;
            else
                    return node->get_cut(best);
        };

        // reorder the cuts
        for (int i = _num_nodes - 1; i != 1; --i)
        {
            if (Node *node = Node::get_node(i); node->is_and() && node->get_ref_num())
            {
                Cut *cut = reorder_cuts(node);
                if (cut)
                    *node->get_best_cut() = *cut;
                else
                    cut = node->get_best_cut();
                for (int m = 0; m != cut->size; ++m)
                    ++Node::get_node(cut->leaves[m])->get_ref_num();

                _map_num_lut  += Cut::get_area_cost(cut);
                _map_num_edge += Cut::get_edge_cost(cut);

                assert(cut->arr <= node->get_required());
            }
        }
    }
    else
    {
        for (int i = _num_nodes - 1; i != 1; --i)
        {
            if (Node *node = Node::get_node(i); node->is_and() && node->get_ref_num())
            {
                Cut *cut = node->get_best_cut();
                for (int m = 0; m != cut->size; ++m)
                    ++Node::get_node(cut->leaves[m])->get_ref_num();

                _map_num_lut  += Cut::get_area_cost(cut);
                _map_num_edge += Cut::get_edge_cost(cut);

                assert(cut->arr <= node->get_required());
            }
        }
    }

    // compute mapping level
    std::vector<uint> node_level(_num_nodes, 0);
    for (int i = 1; i != _num_nodes; ++i)
    {
        uint &level = node_level[i];
        if (Node *node = Node::get_node(i); node->is_and() && node->get_ref_num())
        {
            Cut *cut = node->get_best_cut();
            for (int m = 0; m != cut->size; ++m)
                level = std::max(level, node_level[cut->leaves[m]]);
            level += Cut::get_delay_cost(cut);
        }
        _map_num_level = std::max(_map_num_level, level);
    }

    _first_pass = false;
}

Solution *
FoxMap::create_sol_from_curr_map()
{
    Solution *mapping = new Solution(this, _num_nodes);
    for (int i = 1; i != _num_nodes; ++i)
    {
        Node *node = Node::get_node(i);
        mapping->get_ref_count(i) = node->get_ref_num();
        if (node->is_and() && node->get_ref_num())
        {
            mapping->add(i, node->get_best_cut());
        }
    }
    return mapping;
}

void
FoxMap::compute_required_time()
{
    // reference the best cuts to form mapping
    reference_best_cuts();

    Time required = get_global_required();
    if (required == kMaxTime)
        return;

    for (Node *po : _prim_outputs)
    {
        if (Node *fanin = po->get_fanin0(); fanin && fanin->is_and())
            fanin->set_required(required);
    }

    for (int i = _num_nodes; i != 0; --i)
    {
        Node *node = Node::get_node(i);
        if (!node->get_ref_num() || !node->is_and())
            continue;
        Cut *cut = node->get_best_cut();
        Time req = node->get_required() - Cut::get_delay_cost(cut);
        for (int i = 0; i != cut->size; ++i)
        {
            Node *leaf = Node::get_node(cut->leaves[i]);
            leaf->set_required(std::min(leaf->get_required(), req));
        }
    }
}

void
FoxMap::perform_general_mapping(Algo algo, RankFn fn)
{
    auto mapping_start = clock();

    // set the algorithm and funcs used for cut cost computation
    _algo = algo;
    _cut_rank_enu_fn = fn;

    // enumerate cuts

    if (algo == Algo::Flow && _map_param->parallel)
    {
        // using taskflow to parallize
        tf::Taskflow taskflow;
        std::vector<tf::Task> tasks;
        tasks.reserve(_num_nodes + 10);
        tasks.emplace_back(tf::Task());
        for (int i = 1; i != _num_nodes; ++i)
        {
            Node *node = Node::get_node(i);
            if (node->is_pi() || node->is_and()) {
                tasks.emplace_back(taskflow.emplace([=]() { node->cut_enum(this); }));
            } else {
                tasks.emplace_back(tf::Task());
            }
            assert(tasks.size() == i + 1);
            if (node->is_and()) {
                tasks[node->get_fanin0_id()].precede(tasks.back());
                tasks[node->get_fanin1_id()].precede(tasks.back());
            }
        }
        tf::Executor executor;
        executor.run(taskflow).wait();
    }
    else
    {
        for (int i = 1; i != _num_nodes; ++i)
        {
            Node *node = Node::get_node(i);
            if (node->is_pi() || node->is_and())
                node->cut_enum(this);
        }
    }

    // compute and propagate required times
    compute_required_time();

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
        print_mapping(stage, (mapping_end - mapping_start) / (float)CLOCKS_PER_SEC);
    }

    // perform exact area recovery
    perform_exact_improvement(Algo::Exact, fn);

    // performa cut expandsion to reduce LUT number
    if (_map_param->expand_cut)
        perform_cut_expansion(6);
}

void
FoxMap::perform_exact_improvement(Algo algo, RankFn fn)
{
    auto start = clock();

    for (int i = 1; i != _num_nodes; ++i)
    {
        Node *node = Node::get_node(i);
        if (!node->is_and())
            continue;

        if (node->get_ref_num())
            node->get_best_cut()->rip_mffc();

        Cut *best = node->get_best_cut();
        best->compute_cost(algo);

        assert(best->arr <= node->get_required());
        for (int k = 0; k != node->get_cut_num() - 1; ++k)
        {
            Cut *cut = node->get_cut(k);
            cut->compute_cost(algo);
            if (cut->arr <= node->get_required() && fn(cut, best, RankFnSet::kEpsilon) == 1)
                best = cut;
        }
        node->set_best_cut(best);

        if (node->get_ref_num())
            best->ref_mffc();
    }

    compute_required_time();

    auto end = clock();

    if (_map_param->verbose)
        print_mapping("Im", (end - start) / (float)CLOCKS_PER_SEC);
}

bool
FoxMap::node_fanin_compact0(Node *node, std::vector<int> &front, std::vector<int> &visited)
{
    auto GetFaninCost = [&](Node *n) -> int
    {
        int counter = 0;
        if (n->get_ref_num() == 0)
            --counter;
        if (!n->get_fanin0()->get_mark() && n->get_fanin0()->get_ref_num() == 0)
            ++counter;
        if (!n->get_fanin1()->get_mark() && n->get_fanin1()->get_ref_num() == 0)
            ++counter;
        return counter;
    };

    auto FaninUpdate = [&](Node *n) -> void
    {
        // remove n from front
        std::vector<int> temp;
        for (int id : front)
            if (id != n->get_id())
                temp.push_back(id);
        std::swap(front, temp);
        Node *fanin = n->get_fanin0();
        if (fanin->get_mark() == 0)
        {
            front.push_back(fanin->get_id());
            visited.push_back(fanin->get_id());
            fanin->set_mark(1);
        }

        fanin = n->get_fanin1();
        if (fanin->get_mark() == 0)
        {
            front.push_back(fanin->get_id());
            visited.push_back(fanin->get_id());
            fanin->set_mark(1);
        }
    };

    for (int id : front)
    {
        Node *node = Node::get_node(id);
        if (node->is_pi())
            continue;
        if (!node->get_fanin0()->get_mark() && !node->get_fanin1()->get_mark())
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
FoxMap::node_fanin_compact1(Node *node, std::vector<int> &front, std::vector<int> &visited)
{
    auto GetFaninCost = [&](Node *n) -> int
    {
        int counter = 0;
        if (n->get_ref_num() == 0)
            --counter;
        if (!n->get_fanin0()->get_mark() && n->get_fanin0()->get_ref_num() == 0)
            ++counter;
        if (!n->get_fanin1()->get_mark() && n->get_fanin1()->get_ref_num() == 0)
            ++counter;
        return counter;
    };

    auto FaninUpdate = [&](Node *n) -> void
    {
        /// remove n from front
        std::vector<int> temp;
        for (int id : front)
            if (id != n->get_id())
                temp.push_back(id);
        std::swap(front, temp);
        // update
        Node *fanin = n->get_fanin0();
        if (fanin->get_mark() == 0)
        {
            front.push_back(fanin->get_id());
            visited.push_back(fanin->get_id());
            fanin->set_mark(1);
        }

        fanin = n->get_fanin1();
        if (fanin->get_mark() == 0)
        {
            front.push_back(fanin->get_id());
            visited.push_back(fanin->get_id());
            fanin->set_mark(1);
        }
    };

    for (int id : front)
    {
        Node *node = Node::get_node(id);
        if (node->is_pi())
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
FoxMap::perform_cut_expansion(int lut_size)
{
    auto start = clock();

    auto compute_cut_cost = [](std::vector<int> &nodes) -> int
    {
        int cost = 0;
        for (int idx : nodes)
            if (Node::get_node(idx)->get_ref_num() == 0)
                ++cost;
        return cost;
    };

    auto node_update = [this](Node *node, std::vector<int> &front)
    {
        Cut *cut = node->get_best_cut();
        cut->rip_mffc();
        cut->size = front.size();
        assert(cut->size <= 6);
        cut->sign = 0;
        for (int i = 0; i != front.size(); ++i)
        {
            cut->sign |= GetSign(cut->leaves[i]);
            cut->leaves[i] = front[i];
        }
        std::sort(cut->leaves, cut->leaves + cut->size);
        cut->ref_mffc();
        assert(cut->is_valid());
    };

    for (int i = 1; i != _num_nodes; ++i)
    {
        Node *node = Node::get_node(i);
        if (!node->is_and() || !node->get_ref_num())
            continue;
        std::vector<int> front, front_old, visited;
        Cut *cut = node->get_best_cut();
        Time old_arr = cut->arr;
        cut->rip_mffc();
        Area old_area = cut->ref_mffc();
        // mark the cone nodes
        // improve prepare
        for (int m = 0; m != cut->size; ++m)
        {
            int leaf = cut->leaves[m];
            front.push_back(leaf);
            front_old.push_back(leaf);
            visited.push_back(leaf);
            Node::get_node(leaf)->set_mark(1);
        }
        // mark the nodes in the cone
        cut->mark_cone(node, visited);
        // end prepare

        // rip-up the cone
        cut->rip_mffc();

        // compute initial cost
        int cost_before = compute_cut_cost(front);

        // If_ManImproveNodeFaninCompact
        while (true)
        {
            bool result = false;
            if (node_fanin_compact0(node, front, visited) )
                result = true;
            else if (front.size() < 6 && node_fanin_compact1(node, front, visited))
                result = true;
            else
                result = false;

            assert(front.size() <= 6);

            if (!result)
                break;
        }

        int cost_after = compute_cut_cost(front);
        cut->ref_mffc();
        assert(cost_before >= cost_after);

        for (int id : visited)
            Node::get_node(id)->set_mark(0);

        node_update(node, front);

        cut->arr = cut->compute_arr_time();
        cut->rip_mffc();
        Area new_area = cut->ref_mffc();

        // if there is no gain or required is not met
        // recall the previous one
        if (new_area > old_area || cut->arr > node->get_required())
        {
            node_update(node, front_old);
            cut->rip_mffc();
            new_area = cut->ref_mffc();
            assert(new_area == old_area);
            cut->arr = old_arr;
        }
        else
        {
            cut->compute_truth(node);
        }
        assert(cut->size <= 6);
    }

    compute_required_time();

    auto end = clock();

    if (_map_param->verbose)
        print_mapping("Ep", (end - start) / (float)CLOCKS_PER_SEC);
}

Abc_Ntk_t *
FoxMap::gen_mapped_network(Solution *final_mapping)
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
        if (!Node::get_node(i)->is_and() || final_mapping->get_ref_count(i) == 0)
            continue;
        Cut *cut = final_mapping->get_sol(i);
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
FoxMap::map_to_lut()
{
    initialize();

    // report the graph info
    if (_map_param->verbose)
        printf("mapping graph -- Pi %ld, Po %ld, And %ld\n", num_pi(), num_po(), num_and());

    // set up LUT library
    setup_lib();

    if (_map_param->praetor_premap)
    {
        _premap = 1;
        _cut_set.set_mode(CutSet::PruneMode::IDL);

        RankFn fn;
        if (_map_param->timing_driven())
            fn = RankFnSet::cmp_cut_arr_size_area_edge;
        else if (_map_param->area_driven())
            fn = RankFnSet::cmp_cut_area_edge;
        else
            fn = RankFnSet::cmp_cut_edge_area;
        perform_general_mapping(Algo::Praetor, fn);

        _premap = 0;
        _cut_set.set_mode(CutSet::PruneMode::UL);
    }

        if (_map_param->timing_driven())
    {
        _premap = 1;
        std::vector<RankFn> rank_fn_set = {
            RankFnSet::cmp_cut_arr_size_area_edge,
            RankFnSet::cmp_cut_arr_area_edge,
            RankFnSet::cmp_cut_area_edge
        };
        for (int i = 0; i != 3; ++i)
        {
            if (i)
            {
                for (int i = 1; i != _num_nodes; ++i)
                    Node::get_node(i)->get_ref_num() = _num_refs[i];
            }
            perform_general_mapping(Algo::Flow, rank_fn_set[i]);
        }

        _premap = 0;
    }

    RankFn fn = _map_param->tar == OptTarget::Area ? RankFnSet::cmp_cut_area_edge : RankFnSet::cmp_cut_edge_area;

    for (int i = 0; i != _map_param->flow_pass_num; ++i)
        perform_general_mapping(Algo::Flow, fn);

    for (int i = 0; i != _map_param->exact_pass_num; ++i)
        perform_general_mapping(Algo::Exact, fn);

    // mapping solution
    return gen_mapped_network(create_sol_from_curr_map());
}

} // namespace foxmap

Abc_Ntk_t *
perform_supper_map(Abc_Ntk_t *pAig, supper::Param *param)
{
    auto t0 = std::chrono::high_resolution_clock::now();
    if (Abc_NtkGetChoiceNum(pAig))
    {
        Abc_Ntk_t *pNew = Abc_NtkStrash(pAig = Abc_NtkDup(pAig), 0, 1, 0);
        Abc_NtkDelete(pAig);
        pAig = pNew;
        printf("foxmap: choices in AIG are removed.\n");
    }

    supper::Node::s_const_1      = nullptr;
    supper::Cut ::s_lut_cost_lib = nullptr;
    supper::FoxMap mapper(param, pAig);

    Abc_Ntk_t *result = mapper.map_to_lut();
    auto t1 = std::chrono::high_resolution_clock::now();
    std::cout << "Wall time: "
              << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()
              << " ms" << std::endl;
    return result;
}

} // namespace fox
