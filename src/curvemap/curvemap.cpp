#include "curvemap.h"

#include "base/abc/abc.h"
#include "misc/util/utilTruth.h"

#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fox::curvemap {

// =========================================================================
// CutSolution
// =========================================================================

CutSolution::Rel CutSolution::compare(const CutSolution& rhs, double delta) const {
    double cmp = _area - rhs._area;
    int aa;
    if (cmp < -delta)
        aa = -1;
    else if (cmp > delta)
        aa = 1;
    else
        aa = 0;

    int dd = _delay - rhs._delay;
    if (aa < 0) {
        if (dd <= 0)
            return DOM_L;   // this ≤ rhs in both → this dominates
        else
            return PRE;     // smaller area, larger delay → incomparable, this before
    } else if (aa == 0) {
        if (dd == 0)
            return SAME;
        else if (dd < 0)
            return DOM_L;   // same area, smaller delay → this dominates
        else
            return DOM_R;   // same area, larger delay → rhs dominates
    } else {
        if (dd < 0)
            return SUCC;    // larger area, smaller delay → incomparable, this after
        else
            return DOM_R;   // larger area, not better delay → rhs dominates
    }
}

// =========================================================================
// CutCost
// =========================================================================

void CutCost::insert(const CutSolution& sol) {
    for (size_t i = 0; i < _curve.size();) {
        CutSolution::Rel rel = sol.compare(_curve[i]);
        switch (rel) {
        case CutSolution::PRE:
            _curve.insert(_curve.begin() + (ptrdiff_t)i, sol);
            return;
        case CutSolution::SUCC:
            ++i;
            break;
        case CutSolution::DOM_L:
            _curve.erase(_curve.begin() + (ptrdiff_t)i);
            break; // don't advance i — check next at same position
        case CutSolution::DOM_R:
            return; // sol is dominated, discard
        case CutSolution::SAME:
            return; // already have equivalent
        }
    }
    _curve.push_back(sol);
}

CutCost::Iter CutCost::middle() {
    return _curve.begin() + (ptrdiff_t)(_curve.size() / 2);
}

CutSolution& CutCost::get_sol_for_delay(int target) {
    // _curve is sorted by increasing area (→ decreasing delay)
    // First solution with delay ≤ target is the min-area timing-meeting one
    for (auto& s : _curve)
        if (s.delay() <= target)
            return s;
    return min_delay_sol(); // none meets target → return fastest
}

void CutCost::prune(int nsol) {
    int n = (int)_curve.size();
    int step = n / nsol + 1;
    if (step < 2)
        return;
    std::vector<CutSolution> kept;
    kept.reserve((size_t)nsol);
    for (int i = 0; i < n; ++i) {
        if (i == 0 || i == n - 1 || (i > 0 && i < n - 1 && (i % step) == 0))
            kept.push_back(_curve[i]);
    }
    _curve = std::move(kept);
}

CutCost::Rel CutCost::compare(CutCost& rhs) {
    CutSolution& sa = min_area_sol();
    CutSolution& sd = min_delay_sol();
    CutSolution& ra = rhs.min_area_sol();
    CutSolution& rd = rhs.min_delay_sol();
    CutSolution& sm = *middle();
    CutSolution& rm = *rhs.middle();

    auto a = sa.compare(ra);
    auto d = sd.compare(rd);
    auto m = sm.compare(rm);

    int flag = 0;
    // flag bit 2: this dominates rhs (all 3 points are DOM_L or SAME)
    if ((a == CutSolution::DOM_L || a == CutSolution::SAME) &&
        (d == CutSolution::DOM_L || d == CutSolution::SAME) &&
        (m == CutSolution::DOM_L || m == CutSolution::SAME))
        flag |= 2;
    // flag bit 1: rhs dominates this
    if ((a == CutSolution::DOM_R || a == CutSolution::SAME) &&
        (d == CutSolution::DOM_R || d == CutSolution::SAME) &&
        (m == CutSolution::DOM_R || m == CutSolution::SAME))
        flag |= 1;

    switch (flag) {
    case 2:
        return DOM_L;
    case 1:
        return DOM_R;
    case 3:
        return SAME;
    default:
        // Incomparable — use min-delay for ordering
        if (sd.delay() <= rd.delay())
            return PRE;
        else
            return SUCC;
    }
}

// =========================================================================
// Cut
// =========================================================================

Cut::Cut() {
    _leaves.reserve(6);
}

bool Cut::leaves_contain(const Cut* rhs) const {
    if (size() < rhs->size())
        return false;
    size_t j = 0;
    for (size_t i = 0; i < size() && j < rhs->size(); ++i) {
        if (_leaves[i] == rhs->_leaves[j])
            ++j;
        else if (_leaves[i] > rhs->_leaves[j])
            return false; // a leaf in rhs is not in this
    }
    return j == rhs->size();
}

// =========================================================================
// Curvemap
// =========================================================================

Curvemap::Curvemap(Abc_Ntk_t* pNtk, int K, int nPasses)
    : _pNtk(pNtk), _K(K), _nPasses(nPasses < 1 ? 1 : nPasses) {
    assert(Abc_NtkIsStrash(pNtk));
    assert(K >= 2 && K <= 6);
}

Curvemap::~Curvemap() {
    free_cuts();
}

void Curvemap::free_cuts() {
    for (auto& cl : _cuts)
        for (Cut* c : cl)
            delete c;
    for (auto& cl : _cuts)
        cl.clear();
}

void Curvemap::run() {
    _nObjs = Abc_NtkObjNumMax(_pNtk);

    _cuts.resize((size_t)_nObjs);
    _level.resize((size_t)_nObjs, 0);
    _required.resize((size_t)_nObjs, INT_MAX);
    _is_root.resize((size_t)_nObjs, false);
    _best_cut.resize((size_t)_nObjs, nullptr);
    _est_fanout.resize((size_t)_nObjs, 1.0);
    _rep_area.resize((size_t)_nObjs, 0.0);
    _rep_delay.resize((size_t)_nObjs, 0);

    // Collect PIs and POs
    int i;
    Abc_Obj_t* pPi;
    Abc_NtkForEachPi(_pNtk, pPi, i) {
        _pi_ids.push_back(Abc_ObjId(pPi));
    }
    Abc_Obj_t* pPo;
    Abc_NtkForEachPo(_pNtk, pPo, i) {
        _po_ids.push_back(Abc_ObjId(pPo));
    }

    // Topological order: PI → PO (Abc_NtkForEachNode iterates in topo order)
    Abc_Obj_t* pNode;
    Abc_NtkForEachNode(_pNtk, pNode, i) {
        _topo_order.push_back(pNode);
    }

    // Multi-pass mapping.  Pass 0 uses the area-delay Pareto-curve model
    // with the subcut area-flow formula.  Passes 1+ use a single-point
    // area-flow model whose leaf areas are guided by the fanout estimated
    // from the previous pass's LUT cover.
    for (_pass = 0; _pass < _nPasses; ++_pass) {
        free_cuts(); // drop the previous pass's cuts (no-op on first pass)

        // Reset per-node mapping state for this pass
        std::fill(_required.begin(), _required.end(), INT_MAX);
        std::fill(_is_root.begin(), _is_root.end(), false);
        std::fill(_best_cut.begin(), _best_cut.end(), nullptr);

        if (_pass == 0)
            forward_pass();
        else
            forward_pass_flow();

        backward_pass();

        // Update fanout estimates from this pass's cover for the next pass
        if (_pass + 1 < _nPasses)
            compute_est_fanout();
    }
}

Abc_Ntk_t* Curvemap::mapped_ntk() {
    return build_mapped_ntk();
}

// =========================================================================
// Forward pass
// =========================================================================

void Curvemap::forward_pass() {
    // 1. Initialize PIs
    for (int piId : _pi_ids) {
        _level[piId] = 0;
        init_pi_cut(piId);
    }

    // 2. Cut enumeration for AND nodes (PI → PO topological order)
    for (Abc_Obj_t* pObj : _topo_order) {
        int nId = Abc_ObjId(pObj);
        int f0 = Abc_ObjFaninId(pObj, 0);
        int f1 = Abc_ObjFaninId(pObj, 1);
        _level[nId] = 1 + std::max(_level[f0], _level[f1]);
        cut_enum_node(pObj);
    }

    // 3. Compute _max_arrival from PO drivers' trivial cuts
    _max_arrival = 0;
    for (int poId : _po_ids) {
        Abc_Obj_t* pPo = Abc_NtkObj(_pNtk, poId);
        int drvId = Abc_ObjFaninId(pPo, 0);
        if (!_cuts[drvId].empty()) {
            Cut* triv = _cuts[drvId].front(); // trivial cut is at front
            int min_dly = triv->cost().min_delay_sol().delay();
            if (min_dly > _max_arrival)
                _max_arrival = min_dly;
        }
    }

    // Stats
    int sum_cut = 0, max_cut = 0;
    for (auto& cl : _cuts) {
        int n = (int)cl.size();
        sum_cut += n;
        if (n > max_cut) max_cut = n;
    }
    fprintf(stderr, "curvemap PASS %d: cuts %8d  max/node %3d  avg %5.1f  [curve]\n",
            _pass, sum_cut, max_cut, (double)sum_cut / (double)_topo_order.size());
}

void Curvemap::init_pi_cut(int piId) {
    Cut* triv = new Cut;
    triv->_leaves.push_back(piId);
    triv->set_trivial();
    // PI truth table: variable 0 = 0xAAAA... (1010...)
    triv->_truth = 0xAAAAAAAAAAAAAAAAULL;
    // Unit-delay: area=1.0, delay = level(0) + LUT_DELAY + EDGE_DELAY = 1, level = 1
    triv->_cost.insert(CutSolution(LUT_AREA,
                                   _level[piId] + LUT_DELAY + EDGE_DELAY,
                                   _level[piId] + 1));
    _cuts[piId].push_back(triv);
}

void Curvemap::cut_enum_node(Abc_Obj_t* pNode) {
    int nId   = Abc_ObjId(pNode);
    int f0id  = Abc_ObjFaninId(pNode, 0);
    int f1id  = Abc_ObjFaninId(pNode, 1);
    bool c0   = Abc_ObjFaninC(pNode, 0);
    bool c1   = Abc_ObjFaninC(pNode, 1);

    Abc_Obj_t* fo0 = Abc_ObjFanin(pNode, 0);
    Abc_Obj_t* fo1 = Abc_ObjFanin(pNode, 1);
    int fanout0 = Abc_ObjFanoutNum(fo0);
    int fanout1 = Abc_ObjFanoutNum(fo1);

    auto& cl0 = _cuts[f0id];
    auto& cl1 = _cuts[f1id];

    for (Cut* cut0 : cl0) {
        for (Cut* cut1 : cl1) {
            // Merge leaves (sorted union)
            std::vector<int> merged;
            merged.reserve(cut0->size() + cut1->size());
            auto i0 = cut0->leaf_begin(), e0 = cut0->leaf_end();
            auto i1 = cut1->leaf_begin(), e1 = cut1->leaf_end();
            while (i0 != e0 && i1 != e1) {
                if (*i0 < *i1)
                    merged.push_back(*i0++);
                else if (*i1 < *i0)
                    merged.push_back(*i1++);
                else {
                    merged.push_back(*i0);
                    ++i0;
                    ++i1;
                }
            }
            merged.insert(merged.end(), i0, e0);
            merged.insert(merged.end(), i1, e1);

            if ((int)merged.size() > _K)
                continue;

            // Redundancy check — is there an existing cut whose leaves ⊆ merged?
            if (is_cut_redundant(nId, merged))
                continue;

            // Compute truth table
            uint64_t tt = compute_truth(merged, cut0, cut1, c0, c1);

            // Create merged cut
            Cut* mc = new Cut;
            mc->_leaves = std::move(merged);
            mc->_truth   = tt;

            // Seed with this LUT's own cost
            mc->_cost.insert(CutSolution(LUT_AREA, LUT_DELAY, 1));

            // Combine fanin costs using area-flow
            combine_costs(mc, cut0, fanout0);
            combine_costs(mc, cut1, fanout1);

            mc->_cost.prune(2 * SOL_BOUND); // intermediate prune

            insert_cut(nId, mc);
        }
    }

    // Create the trivial cut for this node
    create_trivial_cut(nId);
}

void Curvemap::combine_costs(Cut* merged, const Cut* subcut, int fanout) {
    CutCost newcost;
    int fout = fanout > 0 ? fanout : 1; // guard (fanout should always be ≥ 1 for AIG)
    for (auto& sol : merged->_cost) {
        for (auto& sub : subcut->_cost) {
            CutSolution nsol = sol;
            // area-flow: (subgraph_cost) / fanout
            nsol.inc_area((sub.area() - LUT_AREA) / (double)fout);
            // delay: max(existing_delay, sub.delay)
            nsol.update_delay(sub.delay());
            nsol.update_level(sub.level());
            newcost.insert(nsol);
        }
    }
    merged->_cost = std::move(newcost);
}

void Curvemap::create_trivial_cut(int nId) {
    Cut* triv = new Cut;
    triv->_leaves.push_back(nId);
    triv->set_trivial();
    triv->_truth = 0xAAAAAAAAAAAAAAAAULL;

    if (_cuts[nId].empty()) {
        // PI-like case (shouldn't happen for AND nodes but keep for safety)
        triv->_cost.insert(CutSolution(LUT_AREA,
                                       _level[nId] + LUT_DELAY + EDGE_DELAY,
                                       _level[nId] + 1));
    } else {
        for (Cut* c : _cuts[nId]) {
            if (c->is_trivial())
                continue;
            for (auto& sol : c->_cost) {
                CutSolution nsol = sol;
                nsol.inc_area(LUT_AREA);
                nsol.inc_delay(LUT_DELAY + EDGE_DELAY);
                nsol.inc_level();
                triv->_cost.insert(nsol);
            }
        }
        triv->_cost.prune(SOL_BOUND);
    }

    // Push to front (as in original pushFrontCut)
    _cuts[nId].insert(_cuts[nId].begin(), triv);
}

// =========================================================================
// Pass 2+ forward pass — single-point area-flow model
//
// Each cut carries a single solution.  The area of a cut is
//   A_cut = 1 + Σ_leaf  A_leaf / est_fanout[leaf]
// where A_leaf is the area of the leaf node's min-delay cut from the
// current pass (stored in _rep_area), and est_fanout comes from the
// previous pass's LUT cover.  The delay is the unit-delay depth.
// =========================================================================

void Curvemap::forward_pass_flow() {
    // 1. Initialize PIs.  A PI is not a LUT, so it contributes 0 area; its
    //    min-delay representative delay is LUT_DELAY (matching the pass-0
    //    trivial-cut convention so consumers see leaf delay = 1).
    for (int piId : _pi_ids) {
        _level[piId] = 0;
        Cut* triv = new Cut;
        triv->_leaves.push_back(piId);
        triv->set_trivial();
        triv->_truth = 0xAAAAAAAAAAAAAAAAULL;
        triv->_cost.insert(CutSolution(0.0, LUT_DELAY + EDGE_DELAY, 1));
        _cuts[piId].push_back(triv);
        _rep_area[piId]  = 0.0;
        _rep_delay[piId] = LUT_DELAY + EDGE_DELAY;
    }

    // 2. Cut enumeration for AND nodes (PI → PO topological order)
    for (Abc_Obj_t* pObj : _topo_order) {
        int nId = Abc_ObjId(pObj);
        int f0 = Abc_ObjFaninId(pObj, 0);
        int f1 = Abc_ObjFaninId(pObj, 1);
        _level[nId] = 1 + std::max(_level[f0], _level[f1]);
        cut_enum_node_flow(pObj);
        update_rep(nId);          // record the min-delay cut as A_leaf/D_leaf
        create_trivial_cut_flow(nId);
    }

    // 3. Compute _max_arrival from PO drivers' trivial cuts
    _max_arrival = 0;
    for (int poId : _po_ids) {
        Abc_Obj_t* pPo = Abc_NtkObj(_pNtk, poId);
        int drvId = Abc_ObjFaninId(pPo, 0);
        if (!_cuts[drvId].empty()) {
            Cut* triv = _cuts[drvId].front();
            int min_dly = triv->cost().min_delay_sol().delay();
            if (min_dly > _max_arrival)
                _max_arrival = min_dly;
        }
    }

    // Stats
    int sum_cut = 0, max_cut = 0;
    for (auto& cl : _cuts) {
        int n = (int)cl.size();
        sum_cut += n;
        if (n > max_cut) max_cut = n;
    }
    fprintf(stderr, "curvemap PASS %d: cuts %8d  max/node %3d  avg %5.1f  [flow]\n",
            _pass, sum_cut, max_cut, (double)sum_cut / (double)_topo_order.size());
}

void Curvemap::cut_enum_node_flow(Abc_Obj_t* pNode) {
    int nId  = Abc_ObjId(pNode);
    int f0id = Abc_ObjFaninId(pNode, 0);
    int f1id = Abc_ObjFaninId(pNode, 1);
    bool c0  = Abc_ObjFaninC(pNode, 0);
    bool c1  = Abc_ObjFaninC(pNode, 1);

    auto& cl0 = _cuts[f0id];
    auto& cl1 = _cuts[f1id];

    for (Cut* cut0 : cl0) {
        for (Cut* cut1 : cl1) {
            // Merge leaves (sorted union)
            std::vector<int> merged;
            merged.reserve(cut0->size() + cut1->size());
            auto i0 = cut0->leaf_begin(), e0 = cut0->leaf_end();
            auto i1 = cut1->leaf_begin(), e1 = cut1->leaf_end();
            while (i0 != e0 && i1 != e1) {
                if (*i0 < *i1)
                    merged.push_back(*i0++);
                else if (*i1 < *i0)
                    merged.push_back(*i1++);
                else {
                    merged.push_back(*i0);
                    ++i0;
                    ++i1;
                }
            }
            merged.insert(merged.end(), i0, e0);
            merged.insert(merged.end(), i1, e1);

            if ((int)merged.size() > _K)
                continue;

            if (is_cut_redundant(nId, merged))
                continue;

            uint64_t tt = compute_truth(merged, cut0, cut1, c0, c1);

            Cut* mc = new Cut;
            mc->_leaves = std::move(merged);
            mc->_truth  = tt;

            // Single-point area-flow cost:
            //   A_cut = 1 + Σ A_leaf / est_fanout[leaf]
            //   D_cut = max(rep_delay[leaf])    (leaf delay already includes
            //                                    its own LUT, so this is the
            //                                    cut's arrival time)
            double area = LUT_AREA;
            int    dly  = 0;
            for (int leaf : mc->_leaves) {
                double ef = _est_fanout[leaf] > 0.0 ? _est_fanout[leaf] : 1.0;
                area += _rep_area[leaf] / ef;
                if (_rep_delay[leaf] > dly)
                    dly = _rep_delay[leaf];
            }
            mc->_cost.insert(CutSolution(area, dly, dly));

            insert_cut(nId, mc);
        }
    }
}

void Curvemap::update_rep(int nId) {
    // A_leaf / D_leaf = area & delay of this node's min-delay cut (over the
    // non-trivial cuts enumerated this pass).  Falls back gracefully if no
    // non-trivial cut exists (degenerate node).
    bool found = false;
    double best_area = 0.0;
    int    best_dly  = INT_MAX;
    for (Cut* c : _cuts[nId]) {
        if (c->is_trivial())
            continue;
        CutSolution& sol = c->cost().min_delay_sol();
        if (!found || sol.delay() < best_dly ||
            (sol.delay() == best_dly && sol.area() < best_area)) {
            best_dly  = sol.delay();
            best_area = sol.area();
            found     = true;
        }
    }
    if (found) {
        _rep_area[nId]  = best_area;
        _rep_delay[nId] = best_dly + LUT_DELAY + EDGE_DELAY;
    } else {
        // No cut (should not happen for AND nodes); treat as a PI-like leaf.
        _rep_area[nId]  = LUT_AREA;
        _rep_delay[nId] = _level[nId] + LUT_DELAY + EDGE_DELAY;
    }
}

void Curvemap::create_trivial_cut_flow(int nId) {
    // Trivial cut: node n viewed as a boundary leaf for its fanouts.  Its
    // single solution mirrors the representative (A_leaf, D_leaf) so that a
    // consumer combining this cut sees leaf area A_leaf and leaf delay D_leaf.
    Cut* triv = new Cut;
    triv->_leaves.push_back(nId);
    triv->set_trivial();
    triv->_truth = 0xAAAAAAAAAAAAAAAAULL;
    triv->_cost.insert(CutSolution(_rep_area[nId], _rep_delay[nId], _rep_delay[nId]));
    _cuts[nId].insert(_cuts[nId].begin(), triv);
}

void Curvemap::compute_est_fanout() {
    // Recount fanout from the current pass's LUT cover.  A node selected as a
    // LUT root gets fanout = number of times it appears as a LUT input; a node
    // not on the cover normalizes to 1.
    std::vector<int> ref((size_t)_nObjs, 0);

    // PO drivers are referenced once each
    for (int poId : _po_ids) {
        Abc_Obj_t* pPo = Abc_NtkObj(_pNtk, poId);
        int drvId = Abc_ObjFaninId(pPo, 0);
        ++ref[drvId];
    }

    // Each selected LUT root references the leaves of its best cut
    for (int i = 0; i < _nObjs; ++i) {
        if (!_is_root[i] || !_best_cut[i])
            continue;
        for (int leaf : _best_cut[i]->_leaves)
            ++ref[leaf];
    }

    for (int i = 0; i < _nObjs; ++i)
        _est_fanout[i] = std::max(1, ref[i]);
}

bool Curvemap::insert_cut(int nId, Cut* cut) {
    auto& cutlist = _cuts[nId];
    bool near_full = (cutlist.size() > (size_t)(2 * CUT_LIMIT / 3));

    for (size_t i = 0; i < cutlist.size();) {
        Cut* old = cutlist[i];

        // Set containment: cut is redundant if its leaves are a superset of old's
        if (cut->leaves_contain(old)) {
            delete cut;
            return false;
        }
        // Old is redundant if its leaves are a superset of cut's
        if (old->leaves_contain(cut)) {
            cutlist.erase(cutlist.begin() + (ptrdiff_t)i);
            delete old;
            continue;
        }

        // Cost curve dominance
        CutCost::Rel rel = cut->cost().compare(old->cost());
        switch (rel) {
        case CutCost::SAME:
            if (cut->size() < old->size()) {
                cutlist.erase(cutlist.begin() + (ptrdiff_t)i);
                delete old;
                continue;
            } else {
                delete cut;
                return false;
            }
        case CutCost::DOM_L:
            // cut cost-dominates old → erase old
            if (cut->size() <= old->size() || near_full) {
                cutlist.erase(cutlist.begin() + (ptrdiff_t)i);
                delete old;
                continue;
            }
            ++i;
            break;
        case CutCost::DOM_R:
            // old cost-dominates cut → discard cut
            if (cut->size() >= old->size() || near_full) {
                delete cut;
                return false;
            }
            ++i;
            break;
        default:
            ++i;
            break;
        }
    }

    if (cutlist.size() < CUT_LIMIT) {
        cutlist.push_back(cut);
        return true;
    }
    delete cut;
    return false;
}

bool Curvemap::is_cut_redundant(int nId, const std::vector<int>& leaves) const {
    for (const Cut* c : _cuts[nId]) {
        // c's leaves ⊆ new leaves → new cut is redundant
        size_t j = 0;
        bool subset = true;
        for (int leaf : c->_leaves) {
            while (j < leaves.size() && leaves[j] < leaf)
                ++j;
            if (j >= leaves.size() || leaves[j] != leaf) {
                subset = false;
                break;
            }
            ++j;
        }
        if (subset)
            return true;
    }
    return false;
}

// =========================================================================
// Truth table computation
// =========================================================================

uint64_t Curvemap::compute_truth(const std::vector<int>& merged_leaves,
                                  const Cut* c0, const Cut* c1,
                                  bool compl0, bool compl1) {
    uint64_t tt0 = c0->_truth;
    uint64_t tt1 = c1->_truth;
    int nv = (int)merged_leaves.size();

    // Permute c0's truth table to match merged cut's variable ordering
    for (int i = nv - 1, k = (int)c0->size() - 1; i >= 0 && k >= 0; i--) {
        if (merged_leaves[i] > c0->leaf(k))
            continue;
        if (k < i)
            Abc_TtSwapVars(&tt0, nv, k, i);
        k--;
    }

    // Permute c1's truth table
    for (int i = nv - 1, k = (int)c1->size() - 1; i >= 0 && k >= 0; i--) {
        if (merged_leaves[i] > c1->leaf(k))
            continue;
        if (k < i)
            Abc_TtSwapVars(&tt1, nv, k, i);
        k--;
    }

    if (compl0) tt0 = ~tt0;
    if (compl1) tt1 = ~tt1;

    return tt0 & tt1;
}

// =========================================================================
// Backward pass
// =========================================================================

void Curvemap::backward_pass() {
    // 1. Seed required times and root marks from POs
    for (int poId : _po_ids) {
        Abc_Obj_t* pPo = Abc_NtkObj(_pNtk, poId);
        int drvId = Abc_ObjFaninId(pPo, 0);
        _required[drvId] = std::min(_required[drvId], _max_arrival);
        _is_root[drvId]  = true;
    }

    // 2. Process in reverse topological order (PO → PI)
    for (int i = (int)_topo_order.size() - 1; i >= 0; i--) {
        Abc_Obj_t* pObj = _topo_order[i];
        int nId = Abc_ObjId(pObj);

        if (!_is_root[nId])
            continue;

        // Select best cut for this root
        Cut* best = cut_sel(nId);
        if (!best) {
            fprintf(stderr, "curvemap error: no cut selected for node %d\n", nId);
            continue;
        }
        _best_cut[nId] = best;

        // Propagate required times to cut leaves
        int leaf_req = _required[nId] - LUT_DELAY;
        for (int leaf : best->_leaves) {
            _required[leaf] = std::min(_required[leaf], leaf_req);
            _is_root[leaf]  = true;
        }
    }

    // Count LUT roots
    int nLuts = 0;
    int maxLvl = 0;
    double totalArea = 0.0;
    for (int i = 0; i < _nObjs; i++) {
        if (_is_root[i] && !Abc_ObjIsCi(Abc_NtkObj(_pNtk, i))) {
            nLuts++;
            if (_best_cut[i]) {
                auto& sol = _best_cut[i]->cost().get_sol_for_delay(_required[i]);
                totalArea += sol.area();
                if (sol.level() > maxLvl) maxLvl = sol.level();
            }
        }
    }

    fprintf(stderr, "curvemap PASS %d: LUTs %8d  depth    %3d  est-area %8.1f  max-arrival %d\n",
            _pass, nLuts, maxLvl, totalArea, _max_arrival);
}

Cut* Curvemap::cut_sel(int nId) {
    int req = _required[nId];
    Cut* best       = nullptr;
    CutSolution best_sol;

    for (Cut* c : _cuts[nId]) {
        if (c->is_trivial())
            continue;

        CutSolution& sol = c->cost().get_sol_for_delay(req);
        if (sol.delay() > req)
            continue;

        if (!best ||
            sol.area() < best_sol.area() ||
            (sol.area() == best_sol.area() && sol.delay() < best_sol.delay())) {
            best     = c;
            best_sol = sol;
        }
    }

    if (!best) {
        // Fallback: pick min-delay cut
        for (Cut* c : _cuts[nId]) {
            if (c->is_trivial())
                continue;
            if (!best || c->cost().min_delay_sol().delay() < best_sol.delay()) {
                best     = c;
                best_sol = c->cost().min_delay_sol();
            }
        }
    }

    return best;
}

// =========================================================================
// Network construction
// =========================================================================

Abc_Ntk_t* Curvemap::build_mapped_ntk() {
    Abc_Ntk_t* ntk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);

    std::vector<Abc_Obj_t*> cache((size_t)_nObjs, nullptr);

    // Create PIs
    for (int piId : _pi_ids) {
        Abc_Obj_t* pPi = Abc_NtkObj(_pNtk, piId);
        Abc_Obj_t* newPi = Abc_NtkCreatePi(ntk);
        char* name = Abc_ObjName(pPi);
        Abc_ObjAssignName(newPi, name, nullptr);
        cache[piId] = newPi;
    }

    // Const-1
    Abc_Obj_t* pConst1Orig = Abc_AigConst1(_pNtk);
    int const1Id = Abc_ObjId(pConst1Orig);
    Abc_Obj_t* const1 = Abc_NtkCreateNodeConst1(ntk);
    cache[const1Id] = const1;

    // Build LUT tree from each PO
    for (int poId : _po_ids) {
        Abc_Obj_t* pPo   = Abc_NtkObj(_pNtk, poId);
        Abc_Obj_t* newPo = Abc_NtkCreatePo(ntk);
        Abc_ObjAssignName(newPo, Abc_ObjName(pPo), nullptr);

        int drvId    = Abc_ObjFaninId(pPo, 0);
        bool drvCmpl = Abc_ObjFaninC(pPo, 0);

        if (!_is_root[drvId]) {
            // Driver is not a mapped LUT root (possibly a direct PI or
            // const).  Wire it directly.
            Abc_Obj_t* drvNode = cache[drvId];
            if (!drvNode) {
                fprintf(stderr, "curvemap error: PO driver %d not cached\n", drvId);
                Abc_ObjAddFanin(newPo, const1);
                continue;
            }
            if (drvCmpl)
                drvNode = Abc_NtkCreateNodeInv(ntk, drvNode);
            Abc_ObjAddFanin(newPo, drvNode);
            continue;
        }

        Abc_Obj_t* lut = create_lut_rec(ntk, drvId, cache);
        if (drvCmpl)
            lut = Abc_NtkCreateNodeInv(ntk, lut);
        Abc_ObjAddFanin(newPo, lut);
    }

    // Remove unused const1
    if (Abc_ObjFanoutNum(const1) == 0)
        Abc_NtkDeleteObj(const1);

    Abc_NtkLogicMakeSimpleCos(ntk, 0);
    if (!Abc_NtkCheck(ntk))
        fprintf(stderr, "curvemap warning: network check failed\n");

    return ntk;
}

Abc_Obj_t* Curvemap::create_lut_rec(Abc_Ntk_t* ntk, int nId,
                                     std::vector<Abc_Obj_t*>& cache) {
    if (cache[nId])
        return cache[nId];

    Abc_Obj_t* pNode = Abc_NtkCreateObj(ntk, ABC_OBJ_NODE);
    cache[nId] = pNode;

    // PIs should already be in cache
    if (Abc_ObjIsCi(Abc_NtkObj(_pNtk, nId))) {
        // PI: just return (already cached above as Abc_NtkCreatePi)
        // Replace the newly created node — PIs are handled specially
        // Actually PIs are created before this is called.  If we reach
        // here for a PI it means the PO is a direct PI (strange but
        // legal).  Just use the PI object.
        return pNode; // already cached
    }

    Cut* best = _best_cut[nId];
    if (!best) {
        fprintf(stderr, "curvemap error: no best cut for root %d\n", nId);
        Abc_ObjAddFanin(pNode, cache[Abc_ObjId(Abc_AigConst1(_pNtk))]);
        pNode->pData = Abc_SopCreateBuf((Mem_Flex_t*)ntk->pManFunc);
        return pNode;
    }

    uint64_t tt = best->_truth;

    if (tt == 0ULL || tt == ~0ULL) {
        // Constant function — wire to const1 + buf/inv
        int c1id = Abc_ObjId(Abc_AigConst1(_pNtk));
        Abc_ObjAddFanin(pNode, cache[c1id]);
        pNode->pData = (tt == 0ULL)
                           ? Abc_SopCreateBuf((Mem_Flex_t*)ntk->pManFunc)
                           : Abc_SopCreateInv((Mem_Flex_t*)ntk->pManFunc);
    } else {
        char* pSop = Abc_SopCreateFromTruth((Mem_Flex_t*)ntk->pManFunc,
                                            (int)best->size(),
                                            (unsigned*)&tt);
        pNode->pData = Abc_SopRegister((Mem_Flex_t*)ntk->pManFunc, pSop);

        // Recursively create fanin LUTs
        for (int leaf : best->_leaves) {
            Abc_Obj_t* pFanin = nullptr;
            if (_is_root[leaf] && _best_cut[leaf]) {
                pFanin = create_lut_rec(ntk, leaf, cache);
            } else {
                pFanin = cache[leaf];
            }
            if (!pFanin) {
                fprintf(stderr, "curvemap error: fanin %d not found\n", leaf);
                continue;
            }
            Abc_ObjAddFanin(pNode, pFanin);
        }
    }

    return pNode;
}

} // namespace fox::curvemap
