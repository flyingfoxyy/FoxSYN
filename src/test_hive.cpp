#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

#include "base/abc/abc.h"
#include "base/io/ioAbc.h"
#include "hive/hive.hpp"
#include "hive/hive_graph.hpp"

namespace {

int g_fail = 0;

void ExpectEq(const char *label, long actual, long expected)
{
    if (actual != expected) {
        std::fprintf(stderr, "FAIL %s: expected %ld, got %ld\n", label, expected, actual);
        ++g_fail;
    }
}

void ExpectTrue(const char *label, bool cond)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL %s\n", label);
        ++g_fail;
    }
}

void ExpectNear(const char *label, double actual, double expected)
{
    if (std::fabs(actual - expected) > 1e-9) {
        std::fprintf(stderr, "FAIL %s: expected %.9f, got %.9f\n", label, expected, actual);
        ++g_fail;
    }
}

// helper: give a node an AND SOP over its current fanins (test_csr3.cpp precedent)
void SetAnd(Abc_Obj_t *n)
{
    auto *pMan = static_cast<Mem_Flex_t *>(n->pNtk->pManFunc);
    if (Abc_ObjFaninNum(n) == 1) n->pData = Abc_SopCreateBuf(pMan);
    else n->pData = Abc_SopCreateAnd(pMan, Abc_ObjFaninNum(n), nullptr);
}

void TestConfigDefaults()
{
    fox::hive::Config cfg;
    ExpectEq("default -N", cfg.num_regions, 20);
    ExpectEq("default -M", cfg.max_nodes, 64);
    ExpectEq("default -I", cfg.max_in, 32);
    ExpectEq("default -O", cfg.max_out, 8);
    ExpectEq("default -K", cfg.lut_k, 6);
    ExpectEq("default -S", cfg.num_seeds, 2000);
    ExpectTrue("default -v off", !cfg.verbose);
}

void TestRunHivePreconditions()
{
    ExpectTrue("null ntk rejected", !fox::hive::RunHive(nullptr, {}).ok);
    Abc_Ntk_t *pStrash = Abc_NtkAlloc(ABC_NTK_STRASH, ABC_FUNC_AIG, 1);
    ExpectTrue("strash ntk rejected", !fox::hive::RunHive(pStrash, {}).ok);
    Abc_NtkDelete(pStrash);
    // PI -> PO only, no internal nodes: ok=true, zero regions (spec 6)
    Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *po = Abc_NtkCreatePo(pNtk);
    Abc_ObjAddFanin(po, a);
    fox::hive::Result r = fox::hive::RunHive(pNtk, {});
    ExpectTrue("no-node ok", r.ok);
    ExpectEq("no-node regions", (long)r.regions.size(), 0);
    Abc_NtkDelete(pNtk);
}

struct Diamond {
    Abc_Ntk_t *ntk;
    Abc_Obj_t *a, *b, *n1, *n2, *n3, *n4, *po;
};

Diamond BuildDiamond()
{
    Diamond d;
    d.ntk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    d.a = Abc_NtkCreatePi(d.ntk);
    d.b = Abc_NtkCreatePi(d.ntk);
    d.n1 = Abc_NtkCreateNode(d.ntk);
    Abc_ObjAddFanin(d.n1, d.a); Abc_ObjAddFanin(d.n1, d.b); SetAnd(d.n1);
    d.n2 = Abc_NtkCreateNode(d.ntk);
    Abc_ObjAddFanin(d.n2, d.n1); SetAnd(d.n2);
    d.n3 = Abc_NtkCreateNode(d.ntk);
    Abc_ObjAddFanin(d.n3, d.n1); SetAnd(d.n3);
    d.n4 = Abc_NtkCreateNode(d.ntk);
    Abc_ObjAddFanin(d.n4, d.n2); Abc_ObjAddFanin(d.n4, d.n3); SetAnd(d.n4);
    d.po = Abc_NtkCreatePo(d.ntk);
    Abc_ObjAddFanin(d.po, d.n4);
    return d;
}

// Global invariant every CombGraph test runs: rank strictly increases on
// every edge (this is what replaces ABC's Level, spec 3.3/5.1).
void CheckRankStrict(const fox::hive::CombGraph &g, const char *label)
{
    for (int v : g.vertices())
        for (int s : g.succs(v))
            if (!(g.rank(s) > g.rank(v))) {
                std::fprintf(stderr, "FAIL %s: rank(%d)=%d !> rank(%d)=%d\n",
                             label, s, g.rank(s), v, g.rank(v));
                ++g_fail;
            }
}

void TestCombGraphDiamond()
{
    Diamond d = BuildDiamond();
    fox::hive::CombGraph g(d.ntk);
    ExpectTrue("acyclic", g.acyclic());
    ExpectEq("4 vertices", (long)g.vertices().size(), 4);
    ExpectTrue("n1 vertex", g.is_vertex(Abc_ObjId(d.n1)));
    ExpectTrue("pi not vertex", !g.is_vertex(Abc_ObjId(d.a)));
    ExpectTrue("po not vertex", !g.is_vertex(Abc_ObjId(d.po)));
    ExpectEq("n1 rank", g.rank(Abc_ObjId(d.n1)), 0);
    ExpectEq("n2 rank", g.rank(Abc_ObjId(d.n2)), 1);
    ExpectEq("n4 rank", g.rank(Abc_ObjId(d.n4)), 2);
    ExpectEq("n1 ext_ins", (long)g.ext_ins(Abc_ObjId(d.n1)).size(), 2);
    ExpectEq("n1 preds", (long)g.preds(Abc_ObjId(d.n1)).size(), 0);
    ExpectEq("n1 succs", (long)g.succs(Abc_ObjId(d.n1)).size(), 2);
    ExpectTrue("n4 ext_out", g.has_ext_out(Abc_ObjId(d.n4)));
    ExpectTrue("n1 no ext_out", !g.has_ext_out(Abc_ObjId(d.n1)));
    CheckRankStrict(g, "diamond rank");
    Abc_NtkDelete(d.ntk);
}

void TestCombGraphDedup()
{
    // nd reads PI a twice; nx reads n1 twice -> one adjacency edge each (spec 4.2)
    Abc_Ntk_t *p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(p);
    Abc_Obj_t *n1 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(n1, a); SetAnd(n1);
    Abc_Obj_t *nd = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(nd, a); Abc_ObjAddFanin(nd, a); SetAnd(nd);
    Abc_Obj_t *nx = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(nx, n1); Abc_ObjAddFanin(nx, n1); SetAnd(nx);
    Abc_Obj_t *po = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po, nx);
    Abc_Obj_t *po2 = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po2, nd);
    fox::hive::CombGraph g(p);
    ExpectEq("dup PI dedup", (long)g.ext_ins(Abc_ObjId(nd)).size(), 1);
    ExpectEq("dup vertex dedup", (long)g.preds(Abc_ObjId(nx)).size(), 1);
    ExpectEq("succs dedup", (long)g.succs(Abc_ObjId(n1)).size(), 1);
    CheckRankStrict(g, "dedup rank");
    Abc_NtkDelete(p);
}

void TestCombGraphLatch()
{
    // n1 -> [BI latch BO] -> n2; latch truncates both directions (spec 5.1)
    Abc_Ntk_t *p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(p);
    Abc_Obj_t *n1 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(n1, a); SetAnd(n1);
    Abc_Obj_t *bo = Abc_NtkAddLatch(p, n1, ABC_INIT_ZERO);  // creates BO<-latch<-BI<-n1
    Abc_Obj_t *n2 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(n2, bo); SetAnd(n2);
    Abc_Obj_t *po = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po, n2);
    fox::hive::CombGraph g(p);
    ExpectTrue("bo not vertex", !g.is_vertex(Abc_ObjId(bo)));
    ExpectEq("n2 preds empty", (long)g.preds(Abc_ObjId(n2)).size(), 0);
    ExpectEq("n2 ext_ins = {BO}", (long)g.ext_ins(Abc_ObjId(n2)).size(), 1);
    ExpectEq("n2 ext_in id", g.ext_ins(Abc_ObjId(n2))[0], (long)Abc_ObjId(bo));
    ExpectTrue("n1 drives BI -> ext_out", g.has_ext_out(Abc_ObjId(n1)));
    ExpectEq("n1 succs empty", (long)g.succs(Abc_ObjId(n1)).size(), 0);
    CheckRankStrict(g, "latch rank");
    Abc_NtkDelete(p);
}

void TestCombGraphBufferRankAndConst()
{
    // Buffer chain: rank must still increment on 1-fanin nodes (ABC's Level
    // would not for barbuf-shaped nodes, spec 3.3); const1 is an ordinary node.
    Abc_Ntk_t *p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(p);
    Abc_Obj_t *b1 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(b1, a); SetAnd(b1);          // 1-fanin buffer
    Abc_Obj_t *b2 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(b2, b1); SetAnd(b2);         // 1-fanin buffer
    Abc_Obj_t *c = Abc_NtkCreateNodeConst1(p);
    Abc_Obj_t *n = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(n, b2); Abc_ObjAddFanin(n, c); SetAnd(n);
    Abc_Obj_t *po = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po, n);
    fox::hive::CombGraph g(p);
    ExpectEq("buf rank +1", g.rank(Abc_ObjId(b2)), g.rank(Abc_ObjId(b1)) + 1);
    ExpectTrue("const1 is vertex", g.is_vertex(Abc_ObjId(c)));
    ExpectTrue("const1 is a preds member of n",
               std::find(g.preds(Abc_ObjId(n)).begin(), g.preds(Abc_ObjId(n)).end(),
                         (int)Abc_ObjId(c)) != g.preds(Abc_ObjId(n)).end());
    CheckRankStrict(g, "buffer rank");
    Abc_NtkDelete(p);
}

} // namespace

int main()
{
    Abc_Start();
    TestConfigDefaults();
    TestRunHivePreconditions();
    TestCombGraphDiamond();
    TestCombGraphDedup();
    TestCombGraphLatch();
    TestCombGraphBufferRankAndConst();
    if (g_fail == 0) std::printf("all hive tests passed\n");
    const int result = g_fail == 0 ? 0 : 1;
    Abc_Stop();
    return result;
}
