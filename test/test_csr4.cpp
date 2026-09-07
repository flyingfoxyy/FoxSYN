#include <cstdio>
#include <vector>
#include "csr4/csr4.hpp"
#include "csr4/csr4_internal.hpp"
#include "base/abc/abc.h"
#include "base/abc/abcPdb.hpp"

namespace {

int g_fail = 0;

void ExpectEqLong(const char *label, long actual, long expected)
{
    if (actual != expected) {
        std::fprintf(stderr, "FAIL %s: expected %ld, got %ld\n", label, expected, actual);
        ++g_fail;
    }
}

void TestCeilLog2()
{
    ExpectEqLong("ceil_log2(1)", fox::csr4::ceil_log2(1), 0);
    ExpectEqLong("ceil_log2(2)", fox::csr4::ceil_log2(2), 1);
    ExpectEqLong("ceil_log2(3)", fox::csr4::ceil_log2(3), 2);
    ExpectEqLong("ceil_log2(4)", fox::csr4::ceil_log2(4), 2);
    ExpectEqLong("ceil_log2(5)", fox::csr4::ceil_log2(5), 3);
    ExpectEqLong("ceil_log2(256)", fox::csr4::ceil_log2(256), 8);
}

// Give a node an AND SOP over its current fanins.
static void SetAnd(Abc_Obj_t *n)
{
    auto *pMan = static_cast<Mem_Flex_t *>(n->pNtk->pManFunc);
    if (Abc_ObjFaninNum(n) == 1) n->pData = Abc_SopCreateBuf(pMan);
    else n->pData = Abc_SopCreateAnd(pMan, Abc_ObjFaninNum(n), nullptr);
}

void TestNodeTruth()
{
    Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *b = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *n0 = Abc_NtkCreateNode(pNtk);
    Abc_ObjAddFanin(n0, a); Abc_ObjAddFanin(n0, b); SetAnd(n0);
    Abc_Obj_t *po = Abc_NtkCreatePo(pNtk); Abc_ObjAddFanin(po, n0);

    uint64_t tt = 0;
    bool ok = fox::csr4::node_truth_u64(n0, tt);
    ExpectEqLong("truth AND ok", ok ? 1 : 0, 1);
    // AND(a,b): minterms 00->0, 01->0, 10->0, 11->1  => low 4 bits = 0b1000 = 8
    ExpectEqLong("truth AND value", (long)(tt & 0xF), 8);

    // XOR via explicit SOP: "10 1\n01 1\n"
    Abc_Obj_t *n1 = Abc_NtkCreateNode(pNtk);
    Abc_ObjAddFanin(n1, a); Abc_ObjAddFanin(n1, b);
    n1->pData = Abc_SopRegister(static_cast<Mem_Flex_t *>(pNtk->pManFunc), "10 1\n01 1\n");
    uint64_t tx = 0;
    ExpectEqLong("truth XOR ok", fox::csr4::node_truth_u64(n1, tx) ? 1 : 0, 1);
    // XOR(a,b): 00->0, 01->1, 10->1, 11->0 => low 4 bits = 0b0110 = 6
    ExpectEqLong("truth XOR value", (long)(tx & 0xF), 6);

    Abc_NtkDelete(pNtk);
}

void TestCollectBoundaryLuts()
{
    // a,b in part 0; x,y in part 1.
    // L0 = AND(a,b,x) in part 1  -> bound {a,b}, qualifies
    // L1 = AND(a,x)   in part 1  -> bound {a},   only 1 crossing fanin, rejected
    Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *b = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *x = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *L0 = Abc_NtkCreateNode(pNtk);
    Abc_ObjAddFanin(L0, a); Abc_ObjAddFanin(L0, b); Abc_ObjAddFanin(L0, x); SetAnd(L0);
    Abc_Obj_t *L1 = Abc_NtkCreateNode(pNtk);
    Abc_ObjAddFanin(L1, a); Abc_ObjAddFanin(L1, x); SetAnd(L1);
    Abc_Obj_t *po0 = Abc_NtkCreatePo(pNtk); Abc_ObjAddFanin(po0, L0);
    Abc_Obj_t *po1 = Abc_NtkCreatePo(pNtk); Abc_ObjAddFanin(po1, L1);

    Abc_ObjSetPartId(a, 0); Abc_ObjSetPartId(b, 0);
    Abc_ObjSetPartId(x, 1);
    Abc_ObjSetPartId(L0, 1); Abc_ObjSetPartId(L1, 1);
    Abc_NtkSetPartStats(pNtk, 2, 0, 0);

    int skipped = 0;
    auto luts = fox::csr4::collect_boundary_luts(pNtk, 1, skipped);
    ExpectEqLong("boundary lut count", (long)luts.size(), 1);
    ExpectEqLong("boundary lut is L0", luts.empty() ? -1 : (long)luts[0].node->Id, (long)L0->Id);
    ExpectEqLong("boundary bound size", luts.empty() ? -1 : (long)luts[0].bound.size(), 2);
    ExpectEqLong("boundary fanin count", luts.empty() ? -1 : (long)luts[0].fanins.size(), 3);
    ExpectEqLong("skipped wide", (long)skipped, 0);

    // the reverse direction has no part-0 node at all
    int skipped2 = 0;
    auto rev = fox::csr4::collect_boundary_luts(pNtk, 0, skipped2);
    ExpectEqLong("reverse boundary count", (long)rev.size(), 0);

    Abc_NtkDelete(pNtk);
}

void TestGroupBoundaryLuts()
{
    // Three synthetic BoundaryLuts (nodes unused by the grouper):
    //   L0 bound {10,11}   L1 bound {11,12}   L2 bound {20,21}
    // L0-L1 share net 11 => one component; L2 is separate.
    std::vector<fox::csr4::BoundaryLut> luts(3);
    luts[0].bound = {10, 11};
    luts[1].bound = {11, 12};
    luts[2].bound = {20, 21};

    auto groups = fox::csr4::group_boundary_luts(luts, 12, 8);
    ExpectEqLong("group count", (long)groups.size(), 2);

    // find the group containing 2 luts
    long big = -1, small = -1;
    for (size_t i = 0; i < groups.size(); ++i) {
        if (groups[i].luts.size() == 2) big = (long)i;
        if (groups[i].luts.size() == 1) small = (long)i;
    }
    ExpectEqLong("has 2-lut group", big >= 0 ? 1 : 0, 1);
    ExpectEqLong("has 1-lut group", small >= 0 ? 1 : 0, 1);
    if (big >= 0)
        ExpectEqLong("big bound union", (long)groups[big].bound_union.size(), 3); // 10,11,12
    if (small >= 0)
        ExpectEqLong("small bound union", (long)groups[small].bound_union.size(), 2); // 20,21

    // maxLuts=1 forces the component to split into singletons
    auto split = fox::csr4::group_boundary_luts(luts, 12, 1);
    ExpectEqLong("split group count", (long)split.size(), 3);
    for (const auto &g : split)
        ExpectEqLong("split group size", (long)g.luts.size(), 1);
}

void TestClassifyBoundNets()
{
    // a,b in part 0. L0=AND(a,b,x) and L1=AND(a,b,y) both in part 1.
    // Group = {L0} only  => a and b each still have L1 as an uncovered sink
    //                      => both land in bkeep.
    // Group = {L0,L1}    => both fully covered => both in bkill.
    Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *b = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *x = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *y = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *L0 = Abc_NtkCreateNode(pNtk);
    Abc_ObjAddFanin(L0, a); Abc_ObjAddFanin(L0, b); Abc_ObjAddFanin(L0, x); SetAnd(L0);
    Abc_Obj_t *L1 = Abc_NtkCreateNode(pNtk);
    Abc_ObjAddFanin(L1, a); Abc_ObjAddFanin(L1, b); Abc_ObjAddFanin(L1, y); SetAnd(L1);
    Abc_Obj_t *po0 = Abc_NtkCreatePo(pNtk); Abc_ObjAddFanin(po0, L0);
    Abc_Obj_t *po1 = Abc_NtkCreatePo(pNtk); Abc_ObjAddFanin(po1, L1);

    Abc_ObjSetPartId(a, 0); Abc_ObjSetPartId(b, 0);
    Abc_ObjSetPartId(x, 1); Abc_ObjSetPartId(y, 1);
    Abc_ObjSetPartId(L0, 1); Abc_ObjSetPartId(L1, 1);
    Abc_NtkSetPartStats(pNtk, 2, 0, 0);

    int skipped = 0;
    auto luts = fox::csr4::collect_boundary_luts(pNtk, 1, skipped);
    ExpectEqLong("classify: lut count", (long)luts.size(), 2);

    std::vector<int> bkill, bkeep;

    fox::csr4::Group solo;
    solo.luts = {0};
    solo.bound_union = luts[0].bound;
    fox::csr4::classify_bound_nets(pNtk, luts, solo, 1, bkill, bkeep);
    ExpectEqLong("solo bkill", (long)bkill.size(), 0);
    ExpectEqLong("solo bkeep", (long)bkeep.size(), 2);

    fox::csr4::Group both;
    both.luts = {0, 1};
    both.bound_union = luts[0].bound;   // {a,b} for either LUT
    fox::csr4::classify_bound_nets(pNtk, luts, both, 1, bkill, bkeep);
    ExpectEqLong("both bkill", (long)bkill.size(), 2);
    ExpectEqLong("both bkeep", (long)bkeep.size(), 0);

    Abc_NtkDelete(pNtk);
}

void TestJointMultiplicity()
{
    // Reproduce docs/csr4.md section 4: L0 = (a^b)&x, L1 = (a^b)|y, a,b in part 0.
    Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *b = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *x = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *y = Abc_NtkCreatePi(pNtk);
    auto *pMan = static_cast<Mem_Flex_t *>(pNtk->pManFunc);

    // L0(a,b,x) = (a^b)&x
    Abc_Obj_t *L0 = Abc_NtkCreateNode(pNtk);
    Abc_ObjAddFanin(L0, a); Abc_ObjAddFanin(L0, b); Abc_ObjAddFanin(L0, x);
    L0->pData = Abc_SopRegister(pMan, "101 1\n011 1\n");

    // L1(a,b,y) = (a^b)|y
    Abc_Obj_t *L1 = Abc_NtkCreateNode(pNtk);
    Abc_ObjAddFanin(L1, a); Abc_ObjAddFanin(L1, b); Abc_ObjAddFanin(L1, y);
    L1->pData = Abc_SopRegister(pMan, "10- 1\n01- 1\n--1 1\n");

    Abc_Obj_t *po0 = Abc_NtkCreatePo(pNtk); Abc_ObjAddFanin(po0, L0);
    Abc_Obj_t *po1 = Abc_NtkCreatePo(pNtk); Abc_ObjAddFanin(po1, L1);

    Abc_ObjSetPartId(a, 0); Abc_ObjSetPartId(b, 0);
    Abc_ObjSetPartId(x, 1); Abc_ObjSetPartId(y, 1);
    Abc_ObjSetPartId(L0, 1); Abc_ObjSetPartId(L1, 1);
    Abc_NtkSetPartStats(pNtk, 2, 0, 0);

    int skipped = 0;
    auto luts = fox::csr4::collect_boundary_luts(pNtk, 1, skipped);
    ExpectEqLong("mu: lut count", (long)luts.size(), 2);

    auto groups = fox::csr4::group_boundary_luts(luts, 12, 8);
    ExpectEqLong("mu: group count", (long)groups.size(), 1);

    std::vector<int> bkill, bkeep;
    fox::csr4::classify_bound_nets(pNtk, luts, groups[0], 1, bkill, bkeep);
    ExpectEqLong("mu: bkill size", (long)bkill.size(), 2);

    long mu = fox::csr4::joint_multiplicity(luts, groups[0], bkill);
    ExpectEqLong("mu joint", mu, 2);
    ExpectEqLong("t from mu", fox::csr4::ceil_log2(mu), 1);

    // Single-LUT group over the same bound set also has mu = 2.
    fox::csr4::Group solo;
    solo.luts = {0};
    solo.bound_union = luts[0].bound;
    long muSolo = fox::csr4::joint_multiplicity(luts, solo, luts[0].bound);
    ExpectEqLong("mu solo L0", muSolo, 2);

    Abc_NtkDelete(pNtk);
}

void TestJointMultiplicityNoWater()
{
    // Saturated case: all four (a,b) values must give four distinct
    // restrictions over x, so mu = 2^|B| and there is no compression.
    // L(a,b,x) = a!b!x + !ab x + ab
    //   ab=00 -> 0    ab=01 -> x    ab=10 -> !x    ab=11 -> 1
    Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *b = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *x = Abc_NtkCreatePi(pNtk);
    auto *pMan = static_cast<Mem_Flex_t *>(pNtk->pManFunc);

    Abc_Obj_t *L = Abc_NtkCreateNode(pNtk);
    Abc_ObjAddFanin(L, a); Abc_ObjAddFanin(L, b); Abc_ObjAddFanin(L, x);
    L->pData = Abc_SopRegister(pMan, "100 1\n011 1\n11- 1\n");
    Abc_Obj_t *po = Abc_NtkCreatePo(pNtk); Abc_ObjAddFanin(po, L);

    Abc_ObjSetPartId(a, 0); Abc_ObjSetPartId(b, 0);
    Abc_ObjSetPartId(x, 1); Abc_ObjSetPartId(L, 1);
    Abc_NtkSetPartStats(pNtk, 2, 0, 0);

    int skipped = 0;
    auto luts = fox::csr4::collect_boundary_luts(pNtk, 1, skipped);
    ExpectEqLong("nowater: lut count", (long)luts.size(), 1);
    fox::csr4::Group g;
    g.luts = {0};
    g.bound_union = luts[0].bound;
    long mu = fox::csr4::joint_multiplicity(luts, g, luts[0].bound);
    ExpectEqLong("nowater mu", mu, 4);
    ExpectEqLong("nowater t", fox::csr4::ceil_log2(mu), 2);

    Abc_NtkDelete(pNtk);
}

void TestKFeasibleAndEvaluate()
{
    // Same fixture as TestJointMultiplicity: L0=(a^b)&x, L1=(a^b)|y.
    Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *b = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *x = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *y = Abc_NtkCreatePi(pNtk);
    auto *pMan = static_cast<Mem_Flex_t *>(pNtk->pManFunc);

    Abc_Obj_t *L0 = Abc_NtkCreateNode(pNtk);
    Abc_ObjAddFanin(L0, a); Abc_ObjAddFanin(L0, b); Abc_ObjAddFanin(L0, x);
    L0->pData = Abc_SopRegister(pMan, "101 1\n011 1\n");
    Abc_Obj_t *L1 = Abc_NtkCreateNode(pNtk);
    Abc_ObjAddFanin(L1, a); Abc_ObjAddFanin(L1, b); Abc_ObjAddFanin(L1, y);
    L1->pData = Abc_SopRegister(pMan, "10- 1\n01- 1\n--1 1\n");
    Abc_Obj_t *po0 = Abc_NtkCreatePo(pNtk); Abc_ObjAddFanin(po0, L0);
    Abc_Obj_t *po1 = Abc_NtkCreatePo(pNtk); Abc_ObjAddFanin(po1, L1);

    Abc_ObjSetPartId(a, 0); Abc_ObjSetPartId(b, 0);
    Abc_ObjSetPartId(x, 1); Abc_ObjSetPartId(y, 1);
    Abc_ObjSetPartId(L0, 1); Abc_ObjSetPartId(L1, 1);
    Abc_NtkSetPartStats(pNtk, 2, 0, 0);

    int skipped = 0;
    auto luts = fox::csr4::collect_boundary_luts(pNtk, 1, skipped);
    auto groups = fox::csr4::group_boundary_luts(luts, 12, 8);
    ExpectEqLong("eval: group count", (long)groups.size(), 1);

    std::vector<int> bkill, bkeep;
    fox::csr4::classify_bound_nets(pNtk, luts, groups[0], 1, bkill, bkeep);
    // t=1: each LUT keeps 1 free slot, so 1+1=2 <= 4. Feasible.
    ExpectEqLong("k feasible t=1 K=4",
                 fox::csr4::check_k_feasible(luts, groups[0], bkill, 1, 4) ? 1 : 0, 1);
    // t=4 would need 4+1=5 slots > 4. Infeasible.
    ExpectEqLong("k infeasible t=4 K=4",
                 fox::csr4::check_k_feasible(luts, groups[0], bkill, 4, 4) ? 1 : 0, 0);

    fox::csr4::Config cfg;
    cfg.lut_size = 6;
    fox::csr4::GroupResult r = fox::csr4::evaluate_group(pNtk, luts, groups[0], 1, cfg);
    ExpectEqLong("eval n_luts",  (long)r.n_luts,  2);
    ExpectEqLong("eval n_bkill", (long)r.n_bkill, 2);
    ExpectEqLong("eval n_bkeep", (long)r.n_bkeep, 0);
    ExpectEqLong("eval mu",      r.mu,            2);
    ExpectEqLong("eval t",       (long)r.t,       1);
    ExpectEqLong("eval feasible",(long)(r.k_feasible ? 1 : 0), 1);
    ExpectEqLong("eval gain",    (long)r.gain,    1);

    Abc_NtkDelete(pNtk);
}

void TestEndToEnd()
{
    // Same two-LUT fixture; RunCsr4 must succeed and leave the network alone.
    Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *b = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *x = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *y = Abc_NtkCreatePi(pNtk);
    auto *pMan = static_cast<Mem_Flex_t *>(pNtk->pManFunc);

    Abc_Obj_t *L0 = Abc_NtkCreateNode(pNtk);
    Abc_ObjAddFanin(L0, a); Abc_ObjAddFanin(L0, b); Abc_ObjAddFanin(L0, x);
    L0->pData = Abc_SopRegister(pMan, "101 1\n011 1\n");
    Abc_Obj_t *L1 = Abc_NtkCreateNode(pNtk);
    Abc_ObjAddFanin(L1, a); Abc_ObjAddFanin(L1, b); Abc_ObjAddFanin(L1, y);
    L1->pData = Abc_SopRegister(pMan, "10- 1\n01- 1\n--1 1\n");
    Abc_Obj_t *po0 = Abc_NtkCreatePo(pNtk); Abc_ObjAddFanin(po0, L0);
    Abc_Obj_t *po1 = Abc_NtkCreatePo(pNtk); Abc_ObjAddFanin(po1, L1);

    Abc_ObjSetPartId(a, 0); Abc_ObjSetPartId(b, 0);
    Abc_ObjSetPartId(x, 1); Abc_ObjSetPartId(y, 1);
    Abc_ObjSetPartId(L0, 1); Abc_ObjSetPartId(L1, 1);
    Abc_NtkSetPartStats(pNtk, 2, 0, 0);

    fox::csr4::Config cfg;
    cfg.verbose = true;
    bool ok = fox::csr4::RunCsr4(pNtk, cfg);
    ExpectEqLong("RunCsr4 ok", ok ? 1 : 0, 1);
    // read-only: still 4 PIs, 2 POs, 2 nodes
    ExpectEqLong("nodes unchanged", (long)Abc_NtkNodeNum(pNtk), 2);
    ExpectEqLong("pis unchanged",   (long)Abc_NtkPiNum(pNtk), 4);

    Abc_NtkDelete(pNtk);
}

} // namespace

int main()
{
    Abc_Start();
    TestCeilLog2();
    TestNodeTruth();
    TestCollectBoundaryLuts();
    TestGroupBoundaryLuts();
    TestClassifyBoundNets();
    TestJointMultiplicity();
    TestJointMultiplicityNoWater();
    TestKFeasibleAndEvaluate();
    TestEndToEnd();
    if (g_fail == 0) std::printf("all csr4 tests passed\n");
    int result = g_fail == 0 ? 0 : 1;
    Abc_Stop();
    return result;
}
