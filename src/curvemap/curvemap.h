#pragma once

#include <cstdint>
#include <cstdlib>
#include <vector>

// Forward declarations (ABC types live in global namespace — ABC_USE_NAMESPACE is off)
struct Abc_Ntk_t_;
struct Abc_Obj_t_;
typedef struct Abc_Ntk_t_ Abc_Ntk_t;
typedef struct Abc_Obj_t_ Abc_Obj_t;

namespace fox::curvemap {

// =========================================================================
// CutSolution — a single point on the area-delay Pareto curve
// =========================================================================
struct CutSolution {
    double _area  = 0.0;
    int    _delay = 0;
    int    _level = 0;

    CutSolution() = default;
    CutSolution(double a, int d, int l) : _area(a), _delay(d), _level(l) {}

    double area()  const { return _area;  }
    int    delay() const { return _delay; }
    int    level() const { return _level; }

    void set_area(double a)  { _area = a;  }
    void set_delay(int d)    { _delay = d; }
    void set_level(int l)    { _level = l; }
    void inc_area(double a)  { _area += a; }
    void inc_delay(int d)    { _delay += d; }
    void inc_level()         { ++_level;  }
    void update_delay(int d) { if (d > _delay) _delay = d; }
    void update_level(int l) { if (l > _level) _level = l; }

    enum Rel { PRE = 0, SUCC, DOM_L, DOM_R, SAME };
    // PRE:   this comes before rhs (incomparable, this has smaller area but larger delay)
    // SUCC:  this comes after  rhs
    // DOM_L: this dominates rhs (area ≤ and delay ≤)
    // DOM_R: rhs dominates this
    // SAME:  identical

    Rel compare(const CutSolution& rhs, double delta = 0.0) const;
};

// =========================================================================
// CutCost — area-delay Pareto curve (solutions sorted by increasing area)
// =========================================================================
class CutCost {
public:
    using Iter      = std::vector<CutSolution>::iterator;
    using ConstIter = std::vector<CutSolution>::const_iterator;

    CutCost() = default;
    CutCost(const CutCost&) = default;
    CutCost& operator=(const CutCost&) = default;

    void insert(const CutSolution& sol);

    size_t size() const { return _curve.size(); }

    Iter      begin()       { return _curve.begin(); }
    Iter      end()         { return _curve.end();   }
    ConstIter begin() const { return _curve.begin(); }
    ConstIter end()   const { return _curve.end();   }

    Iter middle();

    CutSolution& min_area_sol()  { return _curve.front(); }
    CutSolution& min_delay_sol() { return _curve.back();  }
    CutSolution& get_sol_for_delay(int target);

    void prune(int nsol);

    // Curve-level comparison: 3-point (min-area, min-delay, middle) dominance
    enum Rel { PRE = 0, SUCC, DOM_L, DOM_R, SAME };
    Rel compare(CutCost& rhs);

private:
    // Sorted by increasing area (→ decreasing delay, Pareto front)
    std::vector<CutSolution> _curve;
};

// =========================================================================
// Cut — a single K-feasible cut
// =========================================================================
class Curvemap;  // fwd

class Cut {
    friend class Curvemap;
public:
    explicit Cut();

    using LeafIter = std::vector<int>::const_iterator;
    LeafIter leaf_begin() const { return _leaves.begin(); }
    LeafIter leaf_end()   const { return _leaves.end();   }
    int      leaf(int i)  const { return _leaves[i];      }
    size_t   size()       const { return _leaves.size();  }

    bool is_trivial() const { return _is_trivial; }
    void set_trivial()      { _is_trivial = true; }

    uint64_t truth() const { return _truth; }

    // Check if this cut's leaf set includes all leaves of rhs (superset → redundant)
    bool leaves_contain(const Cut* rhs) const;

    CutCost& cost() { return _cost; }
    const CutCost& cost() const { return _cost; }

private:
    std::vector<int> _leaves;     // sorted ObjIds (cut inputs)
    CutCost          _cost;       // Pareto curve of solutions
    uint64_t         _truth = 0;  // truth table (≤ 64 bits, i.e. K ≤ 6)
    bool             _is_trivial = false;
};

// =========================================================================
// Curvemap — the mapper
// =========================================================================
class Curvemap {
public:
    Curvemap(Abc_Ntk_t* pNtk, int K);
    ~Curvemap();

    void run();
    Abc_Ntk_t* mapped_ntk();

    // Parameters (unit-delay model)
    static constexpr double LUT_AREA   = 1.0;
    static constexpr int    LUT_DELAY  = 1;
    static constexpr int    EDGE_DELAY = 0;
    static constexpr int    SOL_BOUND  = 10;
    static constexpr int    CUT_LIMIT  = 30;

private:
    Abc_Ntk_t* _pNtk = nullptr;
    int        _K    = 6;
    int        _nObjs = 0;

    // Per-node data indexed by Abc_ObjId
    std::vector<std::vector<Cut*>> _cuts;       // cut lists
    std::vector<int>               _level;      // logic level
    std::vector<int>               _required;   // required time
    std::vector<bool>              _is_root;    // LUT root flag
    std::vector<Cut*>              _best_cut;   // selected cut

    // Traversal order
    std::vector<Abc_Obj_t*> _topo_order;
    std::vector<int>        _pi_ids;
    std::vector<int>        _po_ids;
    int _max_arrival = 0;

    // ---- Forward pass ----
    void forward_pass();

    void init_pi_cut(int piId);
    void cut_enum_node(Abc_Obj_t* pNode);
    void create_trivial_cut(int nId);

    void combine_costs(Cut* merged, const Cut* subcut, int fanout);

    bool insert_cut(int nId, Cut* cut);
    bool is_cut_redundant(int nId, const std::vector<int>& leaves) const;

    // ---- Truth table ----
    static uint64_t compute_truth(const std::vector<int>& merged_leaves,
                                  const Cut* c0, const Cut* c1,
                                  bool compl0, bool compl1);

    // ---- Backward pass ----
    void backward_pass();
    Cut* cut_sel(int nId);

    // ---- Network construction ----
    Abc_Ntk_t* build_mapped_ntk();
    Abc_Obj_t* create_lut_rec(Abc_Ntk_t* ntk, int nId,
                               std::vector<Abc_Obj_t*>& cache);
};

} // namespace fox::curvemap
