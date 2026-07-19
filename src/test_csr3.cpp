#include <cstdio>
#include <vector>
#include "csr3/csr3.hpp"
#include "csr3/csr3_internal.hpp"
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
    ExpectEqLong("ceil_log2(1)", fox::csr3::ceil_log2(1), 0);
    ExpectEqLong("ceil_log2(2)", fox::csr3::ceil_log2(2), 1);
    ExpectEqLong("ceil_log2(3)", fox::csr3::ceil_log2(3), 2);
    ExpectEqLong("ceil_log2(4)", fox::csr3::ceil_log2(4), 2);
    ExpectEqLong("ceil_log2(5)", fox::csr3::ceil_log2(5), 3);
    ExpectEqLong("ceil_log2(256)", fox::csr3::ceil_log2(256), 8);
}

// helper: give a node an AND SOP over its current fanins
static void SetAnd(Abc_Obj_t *n)
{
    auto *pMan = static_cast<Mem_Flex_t *>(n->pNtk->pManFunc);
    if (Abc_ObjFaninNum(n) == 1) n->pData = Abc_SopCreateBuf(pMan);
    else n->pData = Abc_SopCreateAnd(pMan, Abc_ObjFaninNum(n), nullptr);
}

void TestCollectCrossing()
{
    Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *b = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *n0 = Abc_NtkCreateNode(pNtk);
    Abc_ObjAddFanin(n0, a); Abc_ObjAddFanin(n0, b); SetAnd(n0);
    Abc_Obj_t *n1 = Abc_NtkCreateNode(pNtk);
    Abc_ObjAddFanin(n1, n0); SetAnd(n1);
    Abc_Obj_t *po = Abc_NtkCreatePo(pNtk);
    Abc_ObjAddFanin(po, n1);

    // partition: everything part 0 except n1 part 1
    Abc_ObjSetPartId(a, 0); Abc_ObjSetPartId(b, 0);
    Abc_ObjSetPartId(n0, 0); Abc_ObjSetPartId(n1, 1);
    Abc_NtkSetPartStats(pNtk, 2, 0, 0);

    auto cross01 = fox::csr3::collect_crossing_signals(pNtk, 0);
    ExpectEqLong("cross01 size", (long)cross01.size(), 1);
    ExpectEqLong("cross01 is n0", (long)(cross01.empty()?-1:cross01[0]->Id), (long)n0->Id);
    auto cross10 = fox::csr3::collect_crossing_signals(pNtk, 1);
    ExpectEqLong("cross10 size", (long)cross10.size(), 0);

    Abc_NtkDelete(pNtk);
}

void TestExtractSupport()
{
    Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *b = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *opp = Abc_NtkCreateNode(pNtk);       // opposite-partition feeder
    Abc_ObjAddFanin(opp, a); SetAnd(opp);
    Abc_Obj_t *n0 = Abc_NtkCreateNode(pNtk);
    Abc_ObjAddFanin(n0, b); Abc_ObjAddFanin(n0, opp); SetAnd(n0);
    Abc_Obj_t *po = Abc_NtkCreatePo(pNtk); Abc_ObjAddFanin(po, n0);

    Abc_ObjSetPartId(a, 0); Abc_ObjSetPartId(b, 0);
    Abc_ObjSetPartId(opp, 1);   // opposite partition => leaf boundary
    Abc_ObjSetPartId(n0, 0);
    Abc_NtkSetPartStats(pNtk, 2, 0, 0);

    auto supp = fox::csr3::extract_support_partition_aware(n0, 0);
    // support = { b (PI), opp (opposite-partition leaf) }; NOT a (behind opp)
    ExpectEqLong("supp size", (long)supp.size(), 2);
    bool hasB = false, hasOpp = false, hasA = false;
    for (int id : supp) { if (id==b->Id) hasB=true; if (id==opp->Id) hasOpp=true; if (id==a->Id) hasA=true; }
    ExpectEqLong("supp has b", hasB?1:0, 1);
    ExpectEqLong("supp has opp", hasOpp?1:0, 1);
    ExpectEqLong("supp excludes a (behind opp)", hasA?1:0, 0);

    Abc_NtkDelete(pNtk);
}

void TestGroupByJaccard()
{
    using fox::csr3::Line; using fox::csr3::Group;
    std::vector<Line> lines(3);
    lines[0].support = {1,2,3};
    lines[1].support = {2,3,4};
    lines[2].support = {10,11};
    auto groups = fox::csr3::group_by_jaccard(lines, 30, 16);
    ExpectEqLong("group count", (long)groups.size(), 2);
    // find the group with 2 lines
    int big = -1, small = -1;
    for (size_t i=0;i<groups.size();i++) {
        if (groups[i].lines.size()==2) big=(int)i;
        if (groups[i].lines.size()==1) small=(int)i;
    }
    ExpectEqLong("has 2-line group", big>=0?1:0, 1);
    ExpectEqLong("has 1-line group", small>=0?1:0, 1);
}

void TestBuildConeNtk()
{
    Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *b = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *n0 = Abc_NtkCreateNode(pNtk);
    Abc_ObjAddFanin(n0, a); Abc_ObjAddFanin(n0, b); SetAnd(n0);
    Abc_Obj_t *po = Abc_NtkCreatePo(pNtk); Abc_ObjAddFanin(po, n0);
    Abc_ObjSetPartId(a,0); Abc_ObjSetPartId(b,0); Abc_ObjSetPartId(n0,0);
    Abc_NtkSetPartStats(pNtk, 2, 0, 0);

    std::vector<Abc_Obj_t*> grp = { n0 };
    Abc_Ntk_t *pCone = fox::csr3::build_group_cone_ntk(grp, 0);
    ExpectEqLong("cone PIs", (long)Abc_NtkPiNum(pCone), 2);
    ExpectEqLong("cone POs", (long)Abc_NtkPoNum(pCone), 1);
    ExpectEqLong("cone valid", Abc_NtkCheck(pCone)?1:0, 1);
    Abc_NtkDelete(pCone);
    Abc_NtkDelete(pNtk);
}

void TestSimAndExhaustive()
{
    Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *b = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *n0 = Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(n0,a); Abc_ObjAddFanin(n0,b); SetAnd(n0);
    Abc_Obj_t *n1 = Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(n1,a); Abc_ObjAddFanin(n1,b); SetAnd(n1);
    Abc_Obj_t *p0=Abc_NtkCreatePo(pNtk); Abc_ObjAddFanin(p0,n0);
    Abc_Obj_t *p1=Abc_NtkCreatePo(pNtk); Abc_ObjAddFanin(p1,n1);
    for (Abc_Obj_t*o : {a,b,n0,n1}) Abc_ObjSetPartId(o,0);
    Abc_NtkSetPartStats(pNtk, 2, 0, 0);

    std::vector<Abc_Obj_t*> grp = { n0, n1 };
    Abc_Ntk_t *pCone = fox::csr3::build_group_cone_ntk(grp, 0);
    long mEx = fox::csr3::count_m_exhaustive(pCone, 2);
    ExpectEqLong("exhaustive m (identical lines)", mEx, 2);
    long mSim = fox::csr3::simulate_prefilter(pCone, 2, 4);
    ExpectEqLong("sim lb <= m", (mSim <= 2)?1:0, 1);
    ExpectEqLong("sim lb >= 1", (mSim >= 1)?1:0, 1);
    Abc_NtkDelete(pCone); Abc_NtkDelete(pNtk);
}

void TestConstantCone()
{
    // A 0-fanin constant node in partition 0 with a cross-partition (part 1)
    // fanout is a legitimate crossing line whose cone has zero PIs
    // (is_cone_leaf keeps 0-fanin constant nodes internal, not a PI leaf).
    Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    auto *pMan = static_cast<Mem_Flex_t *>(pNtk->pManFunc);
    Abc_Obj_t *c0 = Abc_NtkCreateNode(pNtk);        // 0-fanin constant-0 node
    c0->pData = Abc_SopCreateConst0(pMan);
    Abc_Obj_t *n1 = Abc_NtkCreateNode(pNtk);        // part-1 consumer of c0
    Abc_ObjAddFanin(n1, c0); SetAnd(n1);
    Abc_Obj_t *po = Abc_NtkCreatePo(pNtk); Abc_ObjAddFanin(po, n1);
    Abc_ObjSetPartId(c0, 0); Abc_ObjSetPartId(n1, 1);
    Abc_NtkSetPartStats(pNtk, 2, 0, 0);

    auto cross = fox::csr3::collect_crossing_signals(pNtk, 0);
    ExpectEqLong("constant node is a crossing signal", (long)cross.size(), 1);

    std::vector<Abc_Obj_t*> grp = { c0 };
    Abc_Ntk_t *pCone = fox::csr3::build_group_cone_ntk(grp, 0);
    ExpectEqLong("constant cone PIs", (long)Abc_NtkPiNum(pCone), 0);
    ExpectEqLong("constant cone POs", (long)Abc_NtkPoNum(pCone), 1);

    // Would SIGFPE (divide by zero on nCi) without the nCi==0 guard.
    ExpectEqLong("count_m_exhaustive on 0-PI cone", fox::csr3::count_m_exhaustive(pCone, 1), 1);
    ExpectEqLong("simulate_prefilter on 0-PI cone", fox::csr3::simulate_prefilter(pCone, 1, 4), 1);

    Abc_NtkDelete(pCone);
    Abc_NtkDelete(pNtk);
}

void TestCountMSat()
{
    // identical lines => m=2
    {
        Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
        Abc_Obj_t *a=Abc_NtkCreatePi(pNtk), *b=Abc_NtkCreatePi(pNtk);
        Abc_Obj_t *n0=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(n0,a);Abc_ObjAddFanin(n0,b);SetAnd(n0);
        Abc_Obj_t *n1=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(n1,a);Abc_ObjAddFanin(n1,b);SetAnd(n1);
        Abc_Obj_t *p0=Abc_NtkCreatePo(pNtk);Abc_ObjAddFanin(p0,n0);
        Abc_Obj_t *p1=Abc_NtkCreatePo(pNtk);Abc_ObjAddFanin(p1,n1);
        for (Abc_Obj_t*o:{a,b,n0,n1}) Abc_ObjSetPartId(o,0);
        Abc_NtkSetPartStats(pNtk,2,0,0);
        std::vector<Abc_Obj_t*> grp={n0,n1};
        Abc_Ntk_t *pCone=fox::csr3::build_group_cone_ntk(grp,0);
        ExpectEqLong("sat m identical", fox::csr3::count_m_sat(pCone,2,100000), 2);
        ExpectEqLong("sat==exhaustive identical",
            fox::csr3::count_m_sat(pCone,2,100000)==fox::csr3::count_m_exhaustive(pCone,2)?1:0, 1);
        Abc_NtkDelete(pCone); Abc_NtkDelete(pNtk);
    }
    // disjoint lines => m=4
    {
        Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
        Abc_Obj_t *a=Abc_NtkCreatePi(pNtk),*b=Abc_NtkCreatePi(pNtk),*c=Abc_NtkCreatePi(pNtk),*d=Abc_NtkCreatePi(pNtk);
        Abc_Obj_t *n0=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(n0,a);Abc_ObjAddFanin(n0,b);SetAnd(n0);
        Abc_Obj_t *n1=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(n1,c);Abc_ObjAddFanin(n1,d);SetAnd(n1);
        Abc_Obj_t *p0=Abc_NtkCreatePo(pNtk);Abc_ObjAddFanin(p0,n0);
        Abc_Obj_t *p1=Abc_NtkCreatePo(pNtk);Abc_ObjAddFanin(p1,n1);
        for (Abc_Obj_t*o:{a,b,c,d,n0,n1}) Abc_ObjSetPartId(o,0);
        Abc_NtkSetPartStats(pNtk,2,0,0);
        std::vector<Abc_Obj_t*> grp={n0,n1};
        Abc_Ntk_t *pCone=fox::csr3::build_group_cone_ntk(grp,0);
        ExpectEqLong("sat m disjoint", fox::csr3::count_m_sat(pCone,2,100000), 4);
        Abc_NtkDelete(pCone); Abc_NtkDelete(pNtk);
    }
}

void TestEndToEnd()
{
    Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a=Abc_NtkCreatePi(pNtk), *b=Abc_NtkCreatePi(pNtk);
    // two identical part-0 nodes, both consumed in part 1 => a redundant crossing pair
    Abc_Obj_t *n0=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(n0,a);Abc_ObjAddFanin(n0,b);SetAnd(n0);
    Abc_Obj_t *n1=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(n1,a);Abc_ObjAddFanin(n1,b);SetAnd(n1);
    Abc_Obj_t *s=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(s,n0);Abc_ObjAddFanin(s,n1);SetAnd(s); // part 1 sink
    Abc_Obj_t *po=Abc_NtkCreatePo(pNtk); Abc_ObjAddFanin(po,s);
    Abc_ObjSetPartId(a,0);Abc_ObjSetPartId(b,0);Abc_ObjSetPartId(n0,0);Abc_ObjSetPartId(n1,0);
    Abc_ObjSetPartId(s,1);
    Abc_NtkSetPartStats(pNtk,2,0,0);

    fox::csr3::Config cfg; cfg.self_check = true;
    bool ok = fox::csr3::RunCsr3(pNtk, cfg);
    ExpectEqLong("RunCsr3 ok", ok?1:0, 1);
    // network unchanged (read-only): still 2 PIs, 1 PO, 3 nodes
    ExpectEqLong("nodes unchanged", (long)Abc_NtkNodeNum(pNtk), 3);
    Abc_NtkDelete(pNtk);
}

} // namespace

int main()
{
    Abc_Start();
    TestCeilLog2();
    TestCollectCrossing();
    TestExtractSupport();
    TestGroupByJaccard();
    TestBuildConeNtk();
    TestSimAndExhaustive();
    TestConstantCone();
    TestCountMSat();
    TestEndToEnd();
    if (g_fail == 0) std::printf("all csr3 tests passed\n");
    int result = g_fail == 0 ? 0 : 1;
    Abc_Stop();
    return result;
}
