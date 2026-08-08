#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

#include "base/abc/abc.h"
#include "base/io/ioAbc.h"
#include "hive/hive.hpp"
#include "hive/region.hpp"
#include "hive/convex.hpp"

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

void TestRegionBoundary()
{
    Diamond d = BuildDiamond();
    fox::hive::CombGraph g(d.ntk);
    fox::hive::Region r(g);
    // {n2,n3,n4}: in = {n1} (n1 drives two members but counts once, spec 2.2)
    r.init({(int)Abc_ObjId(d.n2), (int)Abc_ObjId(d.n3), (int)Abc_ObjId(d.n4)});
    fox::hive::Metrics m = r.metrics(6);
    ExpectEq("in dedup by driver", m.in, 1);
    ExpectEq("out = n4 (drives PO)", m.out, 1);
    ExpectEq("n", m.n, 3);
    ExpectNear("q = 3/2", m.q, 1.5);
    // mixed fanout: {n1,n2}: n1 out (n3 external); absorb n3 -> n1 leaves out
    fox::hive::Region r2(g);
    r2.init({(int)Abc_ObjId(d.n1), (int)Abc_ObjId(d.n2)});
    ExpectEq("n1+n2 out", r2.metrics(6).out, 2);   // n1 (n3 ext), n2 (n4 ext)
    r2.add((int)Abc_ObjId(d.n3));
    ExpectEq("after n3: out", r2.metrics(6).out, 2); // n1 internal now; n2,n3 -> n4 ext
    r2.add((int)Abc_ObjId(d.n4));
    ExpectEq("after n4: out", r2.metrics(6).out, 1); // only n4 (PO)
    ExpectEq("after n4: in", r2.metrics(6).in, 2);   // a, b
    Abc_NtkDelete(d.ntk);
}

void TestRegionConst1Ordinary()
{
    // const1 external: counts in `in` and is an entrance candidate (spec 2.1)
    Abc_Ntk_t *p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(p);
    Abc_Obj_t *c = Abc_NtkCreateNodeConst1(p);
    Abc_Obj_t *n = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(n, a); Abc_ObjAddFanin(n, c); SetAnd(n);
    Abc_Obj_t *po = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po, n);
    fox::hive::CombGraph g(p);
    fox::hive::Region r(g);
    r.init({(int)Abc_ObjId(n)});
    ExpectEq("const1 in `in`", r.metrics(6).in, 2);
    std::vector<int> cands = r.entrance_candidates();
    ExpectEq("const1 is the only entrance candidate", (long)cands.size(), 1);
    ExpectEq("candidate is const1", cands[0], (long)Abc_ObjId(c));
    r.add((int)Abc_ObjId(c));
    ExpectEq("absorbed: in", r.metrics(6).in, 1);
    ExpectEq("absorbed: n", r.metrics(6).n, 2);
    Abc_NtkDelete(p);
}

void TestRegionIncrementalVsRecompute()
{
    Diamond d = BuildDiamond();
    fox::hive::CombGraph g(d.ntk);
    fox::hive::Region r(g);
    r.init({(int)Abc_ObjId(d.n1)});
    // deterministic walk: repeatedly absorb the smallest-id vertex neighbor
    for (int step = 0; step < 3; ++step)
    {
        std::vector<int> cands = r.entrance_candidates();
        for (int om : r.out_members())
            for (int s : g.succs(om))
                if (!r.contains(s))
                    cands.push_back(s);
        std::sort(cands.begin(), cands.end());
        cands.erase(std::unique(cands.begin(), cands.end()), cands.end());
        if (cands.empty()) break;
        r.add(cands[0]);
        fox::hive::Metrics a = r.metrics(6), b = r.recompute(6);
        ExpectEq("inc n == recompute n", a.n, b.n);
        ExpectEq("inc in == recompute in", a.in, b.in);
        ExpectEq("inc out == recompute out", a.out, b.out);
        ExpectNear("inc q == recompute q", a.q, b.q);
    }
    Abc_NtkDelete(d.ntk);
}

void TestLbThreeTerms()
{
    // Term 1 wins: 3 parallel PI->node->PO columns; in=3 out=3, K=6 -> LB=3
    Abc_Ntk_t *p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    std::vector<int> ids;
    for (int i = 0; i < 3; ++i)
    {
        Abc_Obj_t *pi = Abc_NtkCreatePi(p);
        Abc_Obj_t *n = Abc_NtkCreateNode(p);
        Abc_ObjAddFanin(n, pi); SetAnd(n);
        Abc_Obj_t *po = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po, n);
        ids.push_back((int)Abc_ObjId(n));
    }
    {
        fox::hive::CombGraph g(p);
        fox::hive::Region r(g);
        r.init(ids);
        ExpectEq("term1 lb", r.metrics(6).lb, 3);
        ExpectEq("term1 gap", r.metrics(6).gap, 0);
    }
    Abc_NtkDelete(p);

    // Term 2 wins: j1(6 fresh PIs), j2(6 other fresh PIs), both -> PO. K=4:
    // in=12 out=2: t1=2, t2=ceil(10/3)=4, t3=ceil(5/3)=2 -> LB=4
    p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    ids.clear();
    for (int j = 0; j < 2; ++j)
    {
        Abc_Obj_t *n = Abc_NtkCreateNode(p);
        for (int i = 0; i < 6; ++i)
            Abc_ObjAddFanin(n, Abc_NtkCreatePi(p));
        SetAnd(n);
        Abc_Obj_t *po = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po, n);
        ids.push_back((int)Abc_ObjId(n));
    }
    {
        fox::hive::CombGraph g(p);
        fox::hive::Region r(g);
        r.init(ids);
        ExpectEq("term2 lb", r.metrics(4).lb, 4);
    }
    Abc_NtkDelete(p);

    // Term 3 wins: o1,o2 both read the same 6 PIs, both -> PO. K=3:
    // in=6 out=2: t1=2, t2=ceil(4/2)=2, t3=ceil(5/2)=3 -> LB=3
    p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    ids.clear();
    std::vector<Abc_Obj_t *> pis;
    for (int i = 0; i < 6; ++i)
        pis.push_back(Abc_NtkCreatePi(p));
    for (int j = 0; j < 2; ++j)
    {
        Abc_Obj_t *n = Abc_NtkCreateNode(p);
        for (Abc_Obj_t *pi : pis)
            Abc_ObjAddFanin(n, pi);
        SetAnd(n);
        Abc_Obj_t *po = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po, n);
        ids.push_back((int)Abc_ObjId(n));
    }
    {
        fox::hive::CombGraph g(p);
        fox::hive::Region r(g);
        r.init(ids);
        ExpectEq("term3 lb", r.metrics(3).lb, 3);
    }
    Abc_NtkDelete(p);
}

void TestLbEdgeCases()
{
    // (in - out) <= 0 -> term2 is 0, no negative ceiling: p1,p2 both read one
    // PI, both -> PO: in=1 out=2 -> LB = max(2, 0, 0) = 2
    Abc_Ntk_t *p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(p);
    std::vector<int> ids;
    for (int j = 0; j < 2; ++j)
    {
        Abc_Obj_t *n = Abc_NtkCreateNode(p);
        Abc_ObjAddFanin(n, a); SetAnd(n);
        Abc_Obj_t *po = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po, n);
        ids.push_back((int)Abc_ObjId(n));
    }
    {
        fox::hive::CombGraph g(p);
        fox::hive::Region r(g);
        r.init(ids);
        ExpectEq("in<out lb", r.metrics(6).lb, 2);
    }
    Abc_NtkDelete(p);

    // out = 0 (dead region): u1(PI)->u2, u2 no fanout. in=1 out=0:
    // LB = max(0, ceil(1/5)=1, 0 since no outputs) = 1
    p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *b = Abc_NtkCreatePi(p);
    Abc_Obj_t *u1 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(u1, b); SetAnd(u1);
    Abc_Obj_t *u2 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(u2, u1); SetAnd(u2);
    {
        fox::hive::CombGraph g(p);
        fox::hive::Region r(g);
        r.init({(int)Abc_ObjId(u1), (int)Abc_ObjId(u2)});
        ExpectEq("dead out", r.metrics(6).out, 0);
        ExpectEq("dead lb", r.metrics(6).lb, 1);
    }
    Abc_NtkDelete(p);
}

void TestLbFunctionalCounterexample()
{
    // Executable documentation of spec 2.5 limitation 1: LB is NOT a lower
    // bound for functional resynthesis. x = LUT(a,b) ignoring b (SOP "1- 1"),
    // y = LUT(x,c) ignoring c. Structurally in=3 out=1 supp=3, K=2 ->
    // LB = max(1, ceil(2/1), ceil(2/1)) = 2, yet the region computes just `a`
    // (one LUT, even a wire). Do NOT reintroduce a gap<=0 exclusion rule.
    Abc_Ntk_t *p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    auto *pMan = static_cast<Mem_Flex_t *>(p->pManFunc);
    Abc_Obj_t *a = Abc_NtkCreatePi(p);
    Abc_Obj_t *b = Abc_NtkCreatePi(p);
    Abc_Obj_t *c = Abc_NtkCreatePi(p);
    Abc_Obj_t *x = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(x, a); Abc_ObjAddFanin(x, b);
    x->pData = Abc_SopRegister(pMan, "1- 1\n");
    Abc_Obj_t *y = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(y, x); Abc_ObjAddFanin(y, c);
    y->pData = Abc_SopRegister(pMan, "1- 1\n");
    Abc_Obj_t *po = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po, y);
    fox::hive::CombGraph g(p);
    fox::hive::Region r(g);
    r.init({(int)Abc_ObjId(x), (int)Abc_ObjId(y)});
    ExpectEq("counterexample lb", r.metrics(2).lb, 2);  // true functional need: 1
    Abc_NtkDelete(p);
}

void TestLbHandVerifiedOptimal()
{
    // LB <= hand-derived optimal m under model M1-M3 (spec 2.4).
    // (i) 7-input AND as 2-node tree, K=6: in=7 out=1 ->
    //     LB = max(1, ceil(6/5)=2, ceil(6/5)=2) = 2; optimal m = 2.
    Abc_Ntk_t *p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *t1 = Abc_NtkCreateNode(p);
    for (int i = 0; i < 6; ++i)
        Abc_ObjAddFanin(t1, Abc_NtkCreatePi(p));
    SetAnd(t1);
    Abc_Obj_t *t2 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(t2, t1); Abc_ObjAddFanin(t2, Abc_NtkCreatePi(p)); SetAnd(t2);
    Abc_Obj_t *po = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po, t2);
    {
        fox::hive::CombGraph g(p);
        fox::hive::Region r(g);
        r.init({(int)Abc_ObjId(t1), (int)Abc_ObjId(t2)});
        const int lb = r.metrics(6).lb;
        ExpectEq("and7 lb", lb, 2);
        ExpectTrue("and7 lb <= optimal 2", lb <= 2);
    }
    Abc_NtkDelete(p);

    // (ii) Diamond {n1..n4}, K=6: in=2 out=1 -> LB=1; optimal m = 1
    //      (a single LUT of (a,b) computes n4).
    Diamond d = BuildDiamond();
    {
        fox::hive::CombGraph g(d.ntk);
        fox::hive::Region r(g);
        r.init({(int)Abc_ObjId(d.n1), (int)Abc_ObjId(d.n2),
                (int)Abc_ObjId(d.n3), (int)Abc_ObjId(d.n4)});
        const int lb = r.metrics(6).lb;
        ExpectEq("diamond lb", lb, 1);
        ExpectTrue("diamond lb <= optimal 1", lb <= 1);
    }
    Abc_NtkDelete(d.ntk);
}

struct Reconv {
    Abc_Ntk_t *ntk;
    Abc_Obj_t *r1, *v, *r2;
};

Reconv BuildReconv()
{
    Reconv c;
    c.ntk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(c.ntk);
    c.r1 = Abc_NtkCreateNode(c.ntk);
    Abc_ObjAddFanin(c.r1, a); SetAnd(c.r1);
    c.v = Abc_NtkCreateNode(c.ntk);
    Abc_ObjAddFanin(c.v, c.r1); SetAnd(c.v);
    c.r2 = Abc_NtkCreateNode(c.ntk);
    Abc_ObjAddFanin(c.r2, c.r1); Abc_ObjAddFanin(c.r2, c.v); SetAnd(c.r2);
    Abc_Obj_t *po = Abc_NtkCreatePo(c.ntk);
    Abc_ObjAddFanin(po, c.r2);
    return c;
}

void TestClosureSimple()
{
    Reconv c = BuildReconv();
    fox::hive::CombGraph g(c.ntk);
    fox::hive::Region r(g);
    r.init({(int)Abc_ObjId(c.r1), (int)Abc_ObjId(c.r2)});
    ExpectTrue("nonconvex detected by brute",
               !fox::hive::IsConvexBrute(g, r.member_ids()));
    fox::hive::ClosureResult cl =
        fox::hive::ComputeClosure(g, r, {}, fox::hive::kClosureBudget);
    ExpectTrue("closure ok", cl.ok);
    ExpectEq("one violator", (long)cl.violators.size(), 1);
    ExpectEq("violator is v", cl.violators[0], (long)Abc_ObjId(c.v));
    r.add((int)Abc_ObjId(c.v));
    ExpectTrue("convex after absorb", fox::hive::IsConvexBrute(g, r.member_ids()));
    fox::hive::ClosureResult cl2 =
        fox::hive::ComputeClosure(g, r, {}, fox::hive::kClosureBudget);
    ExpectTrue("second closure ok", cl2.ok);
    ExpectEq("second closure no-op", (long)cl2.violators.size(), 0);  // spec 3.2
    Abc_NtkDelete(c.ntk);
}

void TestClosureChainedAndMultiPair()
{
    // r1 -> w1 -> w2 -> r2, r1 -> r2, and r2 -> y -> r3, r2 -> r3:
    // members {r1,r2,r3}, violators {w1,w2,y} in ONE pass (spec 3.2).
    Abc_Ntk_t *p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(p);
    Abc_Obj_t *r1 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(r1, a); SetAnd(r1);
    Abc_Obj_t *w1 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(w1, r1); SetAnd(w1);
    Abc_Obj_t *w2 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(w2, w1); SetAnd(w2);
    Abc_Obj_t *r2 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(r2, r1); Abc_ObjAddFanin(r2, w2); SetAnd(r2);
    Abc_Obj_t *y = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(y, r2); SetAnd(y);
    Abc_Obj_t *r3 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(r3, r2); Abc_ObjAddFanin(r3, y); SetAnd(r3);
    Abc_Obj_t *po = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po, r3);
    fox::hive::CombGraph g(p);
    fox::hive::Region r(g);
    r.init({(int)Abc_ObjId(r1), (int)Abc_ObjId(r2), (int)Abc_ObjId(r3)});
    fox::hive::ClosureResult cl =
        fox::hive::ComputeClosure(g, r, {}, fox::hive::kClosureBudget);
    ExpectTrue("chained ok", cl.ok);
    ExpectEq("three violators", (long)cl.violators.size(), 3);
    // one pass == repeated pass == brute hull
    fox::hive::Region r2x(g);
    std::vector<int> hull = r.member_ids();
    hull.insert(hull.end(), cl.violators.begin(), cl.violators.end());
    r2x.init(hull);
    ExpectTrue("hull convex (brute)", fox::hive::IsConvexBrute(g, r2x.member_ids()));
    fox::hive::ClosureResult again =
        fox::hive::ComputeClosure(g, r2x, {}, fox::hive::kClosureBudget);
    ExpectTrue("hull fixpoint", again.ok && again.violators.empty());
    Abc_NtkDelete(p);
}

void TestClosureLatchNotViolation()
{
    // r1 -> [latch] -> r2: no combinational path, absorbing both is convex.
    Abc_Ntk_t *p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(p);
    Abc_Obj_t *r1 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(r1, a); SetAnd(r1);
    Abc_Obj_t *bo = Abc_NtkAddLatch(p, r1, ABC_INIT_ZERO);
    Abc_Obj_t *r2 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(r2, bo); SetAnd(r2);
    Abc_Obj_t *po = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po, r2);
    fox::hive::CombGraph g(p);
    fox::hive::Region r(g);
    r.init({(int)Abc_ObjId(r1), (int)Abc_ObjId(r2)});
    fox::hive::ClosureResult cl =
        fox::hive::ComputeClosure(g, r, {}, fox::hive::kClosureBudget);
    ExpectTrue("latch path ok", cl.ok);
    ExpectEq("latch path no violators", (long)cl.violators.size(), 0);
    ExpectTrue("latch path convex (brute)", fox::hive::IsConvexBrute(g, r.member_ids()));
    Abc_NtkDelete(p);
}

void TestClosureBudget()
{
    // r1 -> c1..c10 -> r2 plus r1 -> r2: 10 violators; budget 3 -> rejected.
    Abc_Ntk_t *p = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *a = Abc_NtkCreatePi(p);
    Abc_Obj_t *r1 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(r1, a); SetAnd(r1);
    Abc_Obj_t *prev = r1;
    for (int i = 0; i < 10; ++i)
    {
        Abc_Obj_t *ci = Abc_NtkCreateNode(p);
        Abc_ObjAddFanin(ci, prev); SetAnd(ci);
        prev = ci;
    }
    Abc_Obj_t *r2 = Abc_NtkCreateNode(p);
    Abc_ObjAddFanin(r2, r1); Abc_ObjAddFanin(r2, prev); SetAnd(r2);
    Abc_Obj_t *po = Abc_NtkCreatePo(p); Abc_ObjAddFanin(po, r2);
    fox::hive::CombGraph g(p);
    fox::hive::Region r(g);
    r.init({(int)Abc_ObjId(r1), (int)Abc_ObjId(r2)});
    fox::hive::ClosureResult tight = fox::hive::ComputeClosure(g, r, {}, 3);
    ExpectTrue("budget exceeded -> not ok", !tight.ok);
    fox::hive::ClosureResult wide =
        fox::hive::ComputeClosure(g, r, {}, fox::hive::kClosureBudget);
    ExpectTrue("wide ok", wide.ok);
    ExpectEq("ten violators", (long)wide.violators.size(), 10);
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
    TestRegionBoundary();
    TestRegionConst1Ordinary();
    TestRegionIncrementalVsRecompute();
    TestLbThreeTerms();
    TestLbEdgeCases();
    TestLbFunctionalCounterexample();
    TestLbHandVerifiedOptimal();
    TestClosureSimple();
    TestClosureChainedAndMultiPair();
    TestClosureLatchNotViolation();
    TestClosureBudget();
    if (g_fail == 0) std::printf("all hive tests passed\n");
    const int result = g_fail == 0 ? 0 : 1;
    Abc_Stop();
    return result;
}
