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

} // namespace

int main()
{
    TestCeilLog2();
    TestCollectCrossing();
    TestExtractSupport();
    TestGroupByJaccard();
    if (g_fail == 0) std::printf("all csr3 tests passed\n");
    return g_fail == 0 ? 0 : 1;
}
