#include <algorithm>
#include <cstdio>
#include <string>
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

void ExpectEqStr(const char *label, const std::string &actual, const char *expected)
{
    if (actual != expected) {
        std::fprintf(stderr, "FAIL %s: expected \"%s\", got \"%s\"\n", label, expected, actual.c_str());
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

void TestCountMSatTuples()
{
    // identical lines: reachable tuples {00, 11}
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
        std::vector<uint64_t> tuples;
        long m = fox::csr3::count_m_sat(pCone,2,100000,&tuples);
        ExpectEqLong("tuples m identical", m, 2);
        std::sort(tuples.begin(), tuples.end());
        ExpectEqLong("tuples count identical", (long)tuples.size(), 2);
        if (tuples.size()==2) {
            ExpectEqLong("tuples[0]=00", (long)tuples[0], 0);
            ExpectEqLong("tuples[1]=11", (long)tuples[1], 3);
        }
        Abc_NtkDelete(pCone); Abc_NtkDelete(pNtk);
    }
    // complement pair (exercises CO polarity through strash): tuples {01, 10}
    {
        Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
        auto *pMan = static_cast<Mem_Flex_t *>(pNtk->pManFunc);
        Abc_Obj_t *a=Abc_NtkCreatePi(pNtk), *b=Abc_NtkCreatePi(pNtk);
        Abc_Obj_t *n0=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(n0,a);Abc_ObjAddFanin(n0,b);SetAnd(n0);
        Abc_Obj_t *n1=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(n1,n0);
        n1->pData = Abc_SopCreateInv(pMan);
        Abc_Obj_t *p0=Abc_NtkCreatePo(pNtk);Abc_ObjAddFanin(p0,n0);
        Abc_Obj_t *p1=Abc_NtkCreatePo(pNtk);Abc_ObjAddFanin(p1,n1);
        for (Abc_Obj_t*o:{a,b,n0,n1}) Abc_ObjSetPartId(o,0);
        Abc_NtkSetPartStats(pNtk,2,0,0);
        std::vector<Abc_Obj_t*> grp={n0,n1};
        Abc_Ntk_t *pCone=fox::csr3::build_group_cone_ntk(grp,0);
        std::vector<uint64_t> tuples;
        long m = fox::csr3::count_m_sat(pCone,2,100000,&tuples);
        ExpectEqLong("tuples m complement", m, 2);
        std::sort(tuples.begin(), tuples.end());
        ExpectEqLong("tuples count complement", (long)tuples.size(), 2);
        if (tuples.size()==2) {
            ExpectEqLong("tuples[0]=01", (long)tuples[0], 1);
            ExpectEqLong("tuples[1]=10", (long)tuples[1], 2);
        }
        Abc_NtkDelete(pCone); Abc_NtkDelete(pNtk);
    }
    // constant + variable line (exercises constant-driver bits): tuples {00, 10}
    {
        Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
        auto *pMan = static_cast<Mem_Flex_t *>(pNtk->pManFunc);
        Abc_Obj_t *a=Abc_NtkCreatePi(pNtk), *b=Abc_NtkCreatePi(pNtk);
        Abc_Obj_t *c0=Abc_NtkCreateNode(pNtk); c0->pData = Abc_SopCreateConst0(pMan);
        Abc_Obj_t *n0=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(n0,a);Abc_ObjAddFanin(n0,b);SetAnd(n0);
        Abc_Obj_t *p0=Abc_NtkCreatePo(pNtk);Abc_ObjAddFanin(p0,c0);
        Abc_Obj_t *p1=Abc_NtkCreatePo(pNtk);Abc_ObjAddFanin(p1,n0);
        for (Abc_Obj_t*o:{a,b,c0,n0}) Abc_ObjSetPartId(o,0);
        Abc_NtkSetPartStats(pNtk,2,0,0);
        std::vector<Abc_Obj_t*> grp={c0,n0};
        Abc_Ntk_t *pCone=fox::csr3::build_group_cone_ntk(grp,0);
        std::vector<uint64_t> tuples;
        long m = fox::csr3::count_m_sat(pCone,2,100000,&tuples);
        ExpectEqLong("tuples m const+var", m, 2);
        std::sort(tuples.begin(), tuples.end());
        ExpectEqLong("tuples count const+var", (long)tuples.size(), 2);
        if (tuples.size()==2) {
            ExpectEqLong("tuples[0]=00", (long)tuples[0], 0);
            ExpectEqLong("tuples[1]=10", (long)tuples[1], 2);
        }
        Abc_NtkDelete(pCone); Abc_NtkDelete(pNtk);
    }
}

// ---- Phase 1 (re-encoding) tests ----

void TestCodecSops()
{
    using fox::csr3::encoder_sop;
    using fox::csr3::decoder_sop;
    // tuples {00,11}: k=2, m=2, r=1; code 0 -> 00, code 1 -> 11
    {
        std::vector<uint64_t> t = {0, 3};
        ExpectEqStr("enc id bit0", encoder_sop(t, 2, 0), "11 1\n");
        ExpectEqStr("dec id line0", decoder_sop(t, 1, 0), "1 1\n");
        ExpectEqStr("dec id line1", decoder_sop(t, 1, 1), "1 1\n");
    }
    // complement pair {01,10}
    {
        std::vector<uint64_t> t = {1, 2};
        ExpectEqStr("enc c bit0", encoder_sop(t, 2, 0), "01 1\n");
        ExpectEqStr("dec c line0", decoder_sop(t, 1, 0), "0 1\n");
        ExpectEqStr("dec c line1", decoder_sop(t, 1, 1), "1 1\n");
    }
    // one-hot {001,010,100}: k=3, m=3, r=2; codes 0,1,2
    {
        std::vector<uint64_t> t = {1, 2, 4};
        ExpectEqStr("enc oh bit0", encoder_sop(t, 3, 0), "010 1\n");
        ExpectEqStr("enc oh bit1", encoder_sop(t, 3, 1), "001 1\n");
        ExpectEqStr("dec oh line0", decoder_sop(t, 2, 0), "00 1\n");
        ExpectEqStr("dec oh line1", decoder_sop(t, 2, 1), "10 1\n");
        ExpectEqStr("dec oh line2", decoder_sop(t, 2, 2), "01 1\n");
    }
    // empty onset (line0 never 1): for r>=1 vars, zero cubes ("") is ABC's const-0 SOP
    // (nVars==0 uses " 0\n" instead; not applicable here since r=1).
    {
        std::vector<uint64_t> t = {0, 2};
        ExpectEqStr("dec empty onset", decoder_sop(t, 1, 0), "");
    }
}

// identical-pair cone fixture: n0 = n1 = AND(a,b), both POs; returns cone, deletes host
static Abc_Ntk_t *MakeIdenticalPairCone()
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
    Abc_NtkDelete(pNtk);
    return pCone;
}

void TestVerifyCodec()
{
    // good codec on identical-pair cone passes
    {
        Abc_Ntk_t *pCone = MakeIdenticalPairCone();
        std::vector<uint64_t> good = {0, 3};
        ExpectEqLong("verify good codec", fox::csr3::verify_group_codec(pCone, good, 2, 100000)?1:0, 1);
        // corrupted tuple set (reachable 11 dropped => decoder consts 0) must FAIL
        std::vector<uint64_t> bad = {0};
        ExpectEqLong("verify bad codec", fox::csr3::verify_group_codec(pCone, bad, 2, 100000)?1:0, 0);
        Abc_NtkDelete(pCone);
    }
    // const + variable line: exercises empty-onset const-0 decoder output
    {
        Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
        auto *pMan = static_cast<Mem_Flex_t *>(pNtk->pManFunc);
        Abc_Obj_t *a=Abc_NtkCreatePi(pNtk), *b=Abc_NtkCreatePi(pNtk);
        Abc_Obj_t *c0=Abc_NtkCreateNode(pNtk); c0->pData = Abc_SopCreateConst0(pMan);
        Abc_Obj_t *n0=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(n0,a);Abc_ObjAddFanin(n0,b);SetAnd(n0);
        Abc_Obj_t *p0=Abc_NtkCreatePo(pNtk);Abc_ObjAddFanin(p0,c0);
        Abc_Obj_t *p1=Abc_NtkCreatePo(pNtk);Abc_ObjAddFanin(p1,n0);
        for (Abc_Obj_t*o:{a,b,c0,n0}) Abc_ObjSetPartId(o,0);
        Abc_NtkSetPartStats(pNtk,2,0,0);
        std::vector<Abc_Obj_t*> grp={c0,n0};
        Abc_Ntk_t *pCone=fox::csr3::build_group_cone_ntk(grp,0);
        std::vector<uint64_t> t = {0, 2};
        ExpectEqLong("verify const+var codec", fox::csr3::verify_group_codec(pCone, t, 2, 100000)?1:0, 1);
        Abc_NtkDelete(pCone); Abc_NtkDelete(pNtk);
    }
}

void TestApplyCodec()
{
    // n0,n1 identical part-0 lines; s = AND(n0,n1) in part 1; u = AND(n0,a) local part-0 consumer
    Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a=Abc_NtkCreatePi(pNtk), *b=Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *n0=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(n0,a);Abc_ObjAddFanin(n0,b);SetAnd(n0);
    Abc_Obj_t *n1=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(n1,a);Abc_ObjAddFanin(n1,b);SetAnd(n1);
    Abc_Obj_t *s=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(s,n0);Abc_ObjAddFanin(s,n1);SetAnd(s);
    Abc_Obj_t *u=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(u,n0);Abc_ObjAddFanin(u,a);SetAnd(u);
    Abc_Obj_t *p0=Abc_NtkCreatePo(pNtk); Abc_ObjAddFanin(p0,s);
    Abc_Obj_t *p1=Abc_NtkCreatePo(pNtk); Abc_ObjAddFanin(p1,u);
    for (Abc_Obj_t*o:{a,b,n0,n1,u}) Abc_ObjSetPartId(o,0);
    Abc_ObjSetPartId(s,1);
    Abc_NtkSetPartStats(pNtk,2,0,0);

    std::vector<Abc_Obj_t*> lines={n0,n1};
    std::vector<uint64_t> tuples={0,3};
    int r = fox::csr3::apply_group_codec(pNtk, lines, tuples, 0, 1);
    ExpectEqLong("apply r", r, 1);
    ExpectEqLong("apply check", Abc_NtkCheck(pNtk)?1:0, 1);
    // encoder is now the only 0->1 crossing signal
    auto cross = fox::csr3::collect_crossing_signals(pNtk, 0);
    ExpectEqLong("apply crossing after", (long)cross.size(), 1);
    // local part-0 consumer keeps the original driver
    ExpectEqLong("apply local fanout kept", (long)(Abc_ObjFanin0(u)==n0?1:0), 1);
    // part-1 consumer now fed from part-1 decoders only
    {
        Abc_Obj_t *pFanin; int i; bool allDst = true;
        Abc_ObjForEachFanin(s, pFanin, i)
            if ((int)Abc_ObjGetPartId(pFanin) != 1) allDst = false;
        ExpectEqLong("apply s fed by decoders", allDst?1:0, 1);
    }
    // functional: PO0 = a&b, PO1 = (a&b)&a = a&b on all 4 patterns
    for (int p = 0; p < 4; ++p) {
        int model[2] = {p&1, (p>>1)&1};
        int *out = Abc_NtkVerifySimulatePattern(pNtk, model);
        ExpectEqLong("apply po0 func", out[0], model[0]&model[1]);
        ExpectEqLong("apply po1 func", out[1], model[0]&model[1]);
        ABC_FREE(out);
    }
    Abc_NtkDelete(pNtk);
}

void TestApplyCodecRejectsIntroducedCycle()
{
    // n0=AND(a,b) part0; s=BUF(n0) part1 (n0's dst consumer); back=BUF(s) part0
    // (an existing, legal 1->0 crossing line); n1=AND(back,b) part0.
    // Original network is acyclic: n1 depends on back->s->n0, not on itself.
    // Jointly encoding {n0,n1} for 0->1 would repoint s's fanin to a decoder that
    // depends on an encoder fed by {n0,n1} -- creating n1 -> back -> s -> dec -> enc -> n1.
    // apply_group_codec must detect this and refuse (return -1), leaving the
    // network unmodified, rather than installing a network with a comb. loop.
    Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a=Abc_NtkCreatePi(pNtk), *b=Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *n0=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(n0,a);Abc_ObjAddFanin(n0,b);SetAnd(n0);
    Abc_Obj_t *s=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(s,n0); SetAnd(s);
    Abc_Obj_t *back=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(back,s); SetAnd(back);
    Abc_Obj_t *n1=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(n1,back);Abc_ObjAddFanin(n1,b);SetAnd(n1);
    Abc_Obj_t *po=Abc_NtkCreatePo(pNtk); Abc_ObjAddFanin(po,n1);
    for (Abc_Obj_t*o:{a,b,n0,back,n1}) Abc_ObjSetPartId(o,0);
    Abc_ObjSetPartId(s,1);
    Abc_NtkSetPartStats(pNtk,2,0,0);

    ExpectEqLong("pre-apply acyclic", Abc_NtkIsAcyclic(pNtk)?1:0, 1);

    std::vector<Abc_Obj_t*> lines={n0,n1};
    std::vector<uint64_t> tuples={0,1,2};   // m=3, r=2: some gain, distinct enough to be realistic
    int r = fox::csr3::apply_group_codec(pNtk, lines, tuples, 0, 1);
    ExpectEqLong("cycle-introducing apply rejected", r, -1);
    ExpectEqLong("network still acyclic after rejected apply", Abc_NtkIsAcyclic(pNtk)?1:0, 1);
    // s must still be driven by the original n0, not repointed
    ExpectEqLong("s fanin unchanged after rejected apply", (long)(Abc_ObjFanin0(s)==n0?1:0), 1);

    Abc_NtkDelete(pNtk);
}

void TestRunCsr3Encode()
{
    Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a=Abc_NtkCreatePi(pNtk), *b=Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *n0=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(n0,a);Abc_ObjAddFanin(n0,b);SetAnd(n0);
    Abc_Obj_t *n1=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(n1,a);Abc_ObjAddFanin(n1,b);SetAnd(n1);
    Abc_Obj_t *s=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(s,n0);Abc_ObjAddFanin(s,n1);SetAnd(s);
    Abc_Obj_t *po=Abc_NtkCreatePo(pNtk); Abc_ObjAddFanin(po,s);
    Abc_ObjSetPartId(a,0);Abc_ObjSetPartId(b,0);Abc_ObjSetPartId(n0,0);Abc_ObjSetPartId(n1,0);
    Abc_ObjSetPartId(s,1);
    Abc_NtkSetPartStats(pNtk,2,0,0);

    fox::csr3::Config cfg; cfg.encode = true; cfg.self_check = true;
    ExpectEqLong("encode run ok", fox::csr3::RunCsr3(pNtk, cfg)?1:0, 1);
    // 2 crossing wires -> 1 encoder wire; nodes: n0,n1,s + 1 enc + 2 dec
    ExpectEqLong("encode crossing after", (long)fox::csr3::collect_crossing_signals(pNtk,0).size(), 1);
    ExpectEqLong("encode node count", (long)Abc_NtkNodeNum(pNtk), 6);
    for (int p = 0; p < 4; ++p) {
        int model[2] = {p&1, (p>>1)&1};
        int *out = Abc_NtkVerifySimulatePattern(pNtk, model);
        ExpectEqLong("encode po func", out[0], model[0]&model[1]);
        ABC_FREE(out);
    }
    // idempotent: second run finds no more water, changes nothing
    ExpectEqLong("encode rerun ok", fox::csr3::RunCsr3(pNtk, cfg)?1:0, 1);
    ExpectEqLong("encode rerun crossing", (long)fox::csr3::collect_crossing_signals(pNtk,0).size(), 1);
    ExpectEqLong("encode rerun node count", (long)Abc_NtkNodeNum(pNtk), 6);
    Abc_NtkDelete(pNtk);
}

void TestRunCsr3EncodeSkipsCycleGroup()
{
    // n0=AND(a,b) part0, consumed by s (part1): a real 0->1 crossing line.
    // back=BUF(s) part0: an existing, legal 1->0 crossing line.
    // n1 part0 has fanins {back,a,b} but its SOP ("-11 1\n") gives 'back' a
    // don't-care literal, so n1's FUNCTION is exactly AND(a,b) -- identical to
    // n0, giving the {n0,n1} group m=2, k=2, gain=1 -- while 'back' remains a
    // real STRUCTURAL fanin, so n1's support extraction still walks back->s.
    // n1 is consumed by s2 (part1): a second real 0->1 crossing line, grouped
    // with n0 by Jaccard (shared {a,b} support).
    // Applying the joint codec would repoint s's fanin to a decoder fed by an
    // encoder fed by {n0,n1}; n1 depends on back->s, so this closes
    // n1 -> back -> s -> decoder -> encoder -> n1. RunCsr3 must not count this
    // as encoded: the network must stay acyclic and unchanged, and Pdb's
    // cut/hop stats (irrelevant to this run, since it changed nothing) must
    // not be stomped by a rejected-but-mishandled apply_group_codec attempt.
    Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    auto *pMan = static_cast<Mem_Flex_t *>(pNtk->pManFunc);
    Abc_Obj_t *a=Abc_NtkCreatePi(pNtk), *b=Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *n0=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(n0,a);Abc_ObjAddFanin(n0,b);SetAnd(n0);
    Abc_Obj_t *s=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(s,n0); SetAnd(s);
    Abc_Obj_t *back=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(back,s); SetAnd(back);
    Abc_Obj_t *n1=Abc_NtkCreateNode(pNtk);
    Abc_ObjAddFanin(n1,back); Abc_ObjAddFanin(n1,a); Abc_ObjAddFanin(n1,b);
    n1->pData = Abc_SopRegister(pMan, "-11 1\n");   // AND(a,b), 'back' is don't-care
    Abc_Obj_t *s2=Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(s2,n1); SetAnd(s2);
    Abc_Obj_t *po1=Abc_NtkCreatePo(pNtk); Abc_ObjAddFanin(po1,back);
    Abc_Obj_t *po2=Abc_NtkCreatePo(pNtk); Abc_ObjAddFanin(po2,s2);
    for (Abc_Obj_t*o:{a,b,n0,back,n1}) Abc_ObjSetPartId(o,0);
    for (Abc_Obj_t*o:{s,s2}) Abc_ObjSetPartId(o,1);
    Abc_NtkSetPartStats(pNtk,2,0,0);

    ExpectEqLong("cycle-group pre-apply acyclic", Abc_NtkIsAcyclic(pNtk)?1:0, 1);
    auto crossBefore = fox::csr3::collect_crossing_signals(pNtk, 0);
    ExpectEqLong("cycle-group setup has 2 crossing lines", (long)crossBefore.size(), 2);
    int nodesBefore = Abc_NtkNodeNum(pNtk);
    Abc_NtkSetPartStats(pNtk, 2, 42, 7);   // arbitrary sentinel cut/hop this run must not touch

    fox::csr3::Config cfg; cfg.encode = true;
    ExpectEqLong("cycle-group run ok", fox::csr3::RunCsr3(pNtk, cfg)?1:0, 1);

    ExpectEqLong("cycle-group network still acyclic", Abc_NtkIsAcyclic(pNtk)?1:0, 1);
    ExpectEqLong("cycle-group crossing unchanged",
        (long)fox::csr3::collect_crossing_signals(pNtk, 0).size(), (long)crossBefore.size());
    ExpectEqLong("cycle-group no nodes added", (long)Abc_NtkNodeNum(pNtk), nodesBefore);
    ExpectEqLong("cycle-group s fanin unchanged", (long)(Abc_ObjFanin0(s)==n0?1:0), 1);
    ExpectEqLong("cycle-group s2 fanin unchanged", (long)(Abc_ObjFanin0(s2)==n1?1:0), 1);
    // the only group in this network was rejected (would close a cycle), so the
    // run made zero real changes -- Pdb's cut/hop sentinels must survive untouched.
    // A -1 return from apply_group_codec mishandled as "saved k-(-1) wires" (or an
    // Abc_ObjSetPartId call on the encoder/decoder firing before the group is known
    // to be accepted) would make RunCsr3 think it changed the network and stomp these.
    ExpectEqLong("cycle-group cut_size untouched", (long)Abc_NtkPdb(pNtk)->cut_size(), 42);
    ExpectEqLong("cycle-group hop_num untouched", (long)Abc_NtkPdb(pNtk)->hop_num(), 7);

    Abc_NtkDelete(pNtk);
}

void TestRunCsr3EncodeConst()
{
    // constant crossing line: k=1, m=1, r=0 => no wire crosses at all
    Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    auto *pMan = static_cast<Mem_Flex_t *>(pNtk->pManFunc);
    Abc_Obj_t *c0 = Abc_NtkCreateNode(pNtk); c0->pData = Abc_SopCreateConst0(pMan);
    Abc_Obj_t *n1 = Abc_NtkCreateNode(pNtk); Abc_ObjAddFanin(n1, c0); SetAnd(n1);
    Abc_Obj_t *po = Abc_NtkCreatePo(pNtk); Abc_ObjAddFanin(po, n1);
    Abc_ObjSetPartId(c0, 0); Abc_ObjSetPartId(n1, 1);
    Abc_NtkSetPartStats(pNtk, 2, 0, 0);

    fox::csr3::Config cfg; cfg.encode = true;
    ExpectEqLong("const encode run ok", fox::csr3::RunCsr3(pNtk, cfg)?1:0, 1);
    ExpectEqLong("const encode crossing after", (long)fox::csr3::collect_crossing_signals(pNtk,0).size(), 0);
    // n1 now fed by a part-1 constant decoder, not by c0
    ExpectEqLong("const encode fanin repointed", (long)(Abc_ObjFanin0(n1)!=c0?1:0), 1);
    ExpectEqLong("const encode fanin part", (long)Abc_ObjGetPartId(Abc_ObjFanin0(n1)), 1);
    int dummy[1] = {0};
    int *out = Abc_NtkVerifySimulatePattern(pNtk, dummy);
    ExpectEqLong("const encode po func", out[0], 0);
    ABC_FREE(out);
    Abc_NtkDelete(pNtk);
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
    TestCountMSatTuples();
    TestCodecSops();
    TestVerifyCodec();
    TestApplyCodec();
    TestApplyCodecRejectsIntroducedCycle();
    TestEndToEnd();
    TestRunCsr3Encode();
    TestRunCsr3EncodeSkipsCycleGroup();
    TestRunCsr3EncodeConst();
    if (g_fail == 0) std::printf("all csr3 tests passed\n");
    int result = g_fail == 0 ? 0 : 1;
    Abc_Stop();
    return result;
}
