#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "base/abc/abc.h"
#include "base/io/ioAbc.h"
#include "base/main/main.h"
#include "fmpart/abc_wrapper.hpp"
#include "fmpart/fm_buckets.hpp"
#include "fmpart/fmpart.hpp"

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

void TestBucketsBasic()
{
    fox::fmpart::GainBuckets b;
    b.reset(6, 4);
    ExpectTrue("empty at start", b.empty(0) && b.empty(1));
    b.insert(0, 0, 3);
    b.insert(1, 0, 1);
    b.insert(2, 0, -2);
    b.insert(3, 1, 4);
    ExpectEq("max side0", b.max_gain(0), 3);
    ExpectEq("max side1", b.max_gain(1), 4);
    ExpectTrue("contains 1", b.contains(1));
    b.erase(0);
    ExpectEq("max after erase", b.max_gain(0), 1);
    ExpectTrue("no 0", !b.contains(0));
    b.update_gain(1, -4);
    ExpectEq("max after update", b.max_gain(0), -2);
    ExpectEq("gain_of", b.gain_of(1), -4);
    ExpectEq("side_of", b.side_of(1), 0);
    b.erase(1);
    b.erase(2);
    ExpectTrue("side0 empty", b.empty(0));
    ExpectTrue("side1 nonempty", !b.empty(1));
    ExpectEq("consistency", b.check_consistency(), 0);
}

void TestBucketsFindTop()
{
    fox::fmpart::GainBuckets b;
    b.reset(4, 5);
    b.insert(0, 0, 5);
    b.insert(1, 0, 5);
    b.insert(2, 0, 2);
    // Both gain-5 vertices infeasible -> falls through to gain 2
    int v = b.find_top(0, [](int u) { return u == 2; });
    ExpectEq("find_top skips infeasible", v, 2);
    ExpectEq("max pointer intact", b.max_gain(0), 5);   // must not drop past a non-empty bucket
    v = b.find_top(0, [](int) { return false; });
    ExpectEq("find_top none", v, (long)fox::fmpart::GainBuckets::kNone);
    ExpectEq("consistency2", b.check_consistency(), 0);
}

void TestBucketsDegenerate()
{
    fox::fmpart::GainBuckets b;
    b.reset(0, 0);                       // empty graph
    ExpectTrue("empty graph buckets", b.empty(0) && b.empty(1));
    b.reset(2, 0);                       // gmax 0: only legal gain is 0
    b.insert(0, 0, 0);
    ExpectEq("gmax0 max", b.max_gain(0), 0);
    ExpectEq("gmax0 consistency", b.check_consistency(), 0);
}

// Minimal graph type for a second template instantiation (spec §6.1)
struct SimpleHypergraph {
    int nv = 0;
    std::vector<std::vector<int>> pins;      // pin list per net, no duplicates
    std::vector<int> vweights;               // empty = all 1
    std::vector<int> nweights;               // empty = all 1

    int num_vertices() const { return nv; }
    int num_nets() const { return (int)pins.size(); }
    int vertex_weight(int v) const { return vweights.empty() ? 1 : vweights[v]; }
    int net_weight(int e) const { return nweights.empty() ? 1 : nweights[e]; }
    const std::vector<int> &pins_of(int e) const { return pins[e]; }
};

struct NotAGraph {
    int num_vertices() const { return 0; }
};

static_assert(fox::fmpart::FMHypergraph<SimpleHypergraph>);
static_assert(!fox::fmpart::FMHypergraph<NotAGraph>);

// Independent cut recompute on the test side as a check against incremental values (spec §6.2)
int RefCut(const SimpleHypergraph &g, const std::vector<uint8_t> &part)
{
    int cut = 0;
    for (int e = 0; e < g.num_nets(); ++e) {
        bool s0 = false, s1 = false;
        for (int v : g.pins[e])
            (part[v] ? s1 : s0) = true;
        if (s0 && s1)
            cut += g.net_weight(e);
    }
    return cut;
}

// 8 vertices, two clusters + one bridge; optimal 2-way cut = 1 (spec §6.3.1)
SimpleHypergraph TwoClusters()
{
    SimpleHypergraph g;
    g.nv = 8;
    g.pins = {{0,1},{1,2},{2,3},{0,2},{1,3},{4,5},{5,6},{6,7},{4,6},{5,7},{3,4}};
    return g;
}

void TestEchoWithZeroPasses()
{
    SimpleHypergraph g = TwoClusters();
    fox::fmpart::Config cfg;
    cfg.max_passes = 0;
    fox::fmpart::FMPart<SimpleHypergraph> fm(g, cfg);
    const std::vector<uint8_t> init = {0,0,0,0,1,1,1,1};
    auto r = fm.run(init);
    ExpectEq("echo cut", r.cut, 1);
    ExpectEq("echo initial_cut", r.initial_cut, 1);
    ExpectEq("echo ref", RefCut(g, r.part), r.cut);
    ExpectEq("echo passes", r.passes, 0);
    ExpectTrue("echo balanced", r.balanced);
    for (int v = 0; v < 8; ++v)
        ExpectEq("echo part", r.part[v], v < 4 ? 0 : 1);
}

void TestFixedOverridesInit()
{
    SimpleHypergraph g = TwoClusters();
    const std::vector<uint8_t> init = {0,0,0,0,1,1,1,1};
    std::vector<int8_t> fixed(8, -1);
    fixed[0] = 1;
    fox::fmpart::Config cfg;
    cfg.max_passes = 0;
    fox::fmpart::FMPart<SimpleHypergraph> fm(g, cfg);
    auto r = fm.run(init, fixed);
    ExpectEq("fixed wins over init", r.part[0], 1);
    ExpectEq("others follow init", r.part[1], 0);
    ExpectEq("cut echoes input", r.cut, r.initial_cut);
    ExpectEq("ref agrees", RefCut(g, r.part), r.cut);
}

void TestRandomInitBalanced()
{
    SimpleHypergraph g;
    g.nv = 9;
    g.pins = {{0,1,2},{3,4,5},{6,7,8},{0,4,8}};
    fox::fmpart::Config cfg;
    cfg.max_passes = 0;
    fox::fmpart::FMPart<SimpleHypergraph> fm(g, cfg);
    auto r = fm.run();
    ExpectTrue("random init balanced", r.balanced);
    int w1 = 0;
    for (auto p : r.part) w1 += p;
    ExpectTrue("both sides used", w1 > 0 && w1 < 9);
    std::vector<int8_t> fixed(9, -1);
    fixed[2] = 1; fixed[5] = 1; fixed[6] = 0;
    auto rf = fm.run({}, fixed);
    ExpectEq("fx2", rf.part[2], 1);
    ExpectEq("fx5", rf.part[5], 1);
    ExpectEq("fx6", rf.part[6], 0);
}

void TestRunReuse()
{
    SimpleHypergraph g = TwoClusters();
    fox::fmpart::Config cfg;
    fox::fmpart::FMPart<SimpleHypergraph> fm(g, cfg);
    auto r1 = fm.run();
    auto r2 = fm.run();
    ExpectTrue("deterministic across runs", r1.part == r2.part && r1.cut == r2.cut);
}

void TestDegenerate()
{
    fox::fmpart::Config cfg;
    {
        SimpleHypergraph g;                          // empty graph
        fox::fmpart::FMPart<SimpleHypergraph> fm(g, cfg);
        auto r = fm.run();
        ExpectEq("empty cut", r.cut, 0);
        ExpectTrue("empty part", r.part.empty());
    }
    {
        SimpleHypergraph g;                          // single vertex
        g.nv = 1;
        fox::fmpart::FMPart<SimpleHypergraph> fm(g, cfg);
        auto r = fm.run();
        ExpectEq("single cut", r.cut, 0);
        ExpectTrue("single balanced", r.balanced);
    }
    {
        SimpleHypergraph g;                          // all 1-pin nets
        g.nv = 3;
        g.pins = {{0},{1},{2}};
        fox::fmpart::FMPart<SimpleHypergraph> fm(g, cfg);
        auto r = fm.run();
        ExpectEq("1-pin nets never cut", r.cut, 0);
    }
    {
        SimpleHypergraph g;                          // no nets
        g.nv = 4;
        fox::fmpart::FMPart<SimpleHypergraph> fm(g, cfg);
        auto r = fm.run();
        ExpectEq("no nets cut", r.cut, 0);
        ExpectTrue("no nets balanced", r.balanced);
    }
}

void TestKnownOptimal()
{
    SimpleHypergraph g = TwoClusters();
    fox::fmpart::Config cfg;
    cfg.self_check = true;
    fox::fmpart::FMPart<SimpleHypergraph> fm(g, cfg);
    const std::vector<uint8_t> init = {0,1,0,1,0,1,0,1};   // alternating start, cut = 9
    auto r = fm.run(init);
    ExpectEq("self-check clean", r.self_check_failures, 0);
    ExpectEq("optimal cut", r.cut, 1);
    ExpectTrue("balanced", r.balanced);
    ExpectTrue("cluster A together",
               r.part[0] == r.part[1] && r.part[1] == r.part[2] && r.part[2] == r.part[3]);
    ExpectTrue("cluster B together",
               r.part[4] == r.part[5] && r.part[5] == r.part[6] && r.part[6] == r.part[7]);
    ExpectTrue("clusters apart", r.part[0] != r.part[4]);
    ExpectEq("ref cut agrees", RefCut(g, r.part), r.cut);
}

void TestMonotonicPasses()
{
    // Observe pass-by-pass: max_passes=1, feed previous part as next init;
    // semantically equivalent to consecutive passes (spec §6.3.5)
    SimpleHypergraph g = TwoClusters();
    fox::fmpart::Config cfg;
    cfg.max_passes = 1;
    cfg.self_check = true;
    fox::fmpart::FMPart<SimpleHypergraph> fm(g, cfg);
    std::vector<uint8_t> cur = {0,1,0,1,0,1,0,1};
    int prev_cut = RefCut(g, cur);
    for (int p = 0; p < 5; ++p) {
        auto r = fm.run(cur);
        ExpectEq("pass self-check", r.self_check_failures, 0);
        ExpectTrue("cut monotonic non-increasing", r.cut <= prev_cut);
        ExpectEq("pass ref agrees", RefCut(g, r.part), r.cut);
        prev_cut = r.cut;
        cur = r.part;
    }
    ExpectEq("converged to optimum", prev_cut, 1);
}

void TestFixedPins()
{
    SimpleHypergraph g = TwoClusters();
    std::vector<int8_t> fixed(8, -1);
    fixed[0] = 1;                        // pin against the natural cluster direction
    fixed[7] = 0;
    fox::fmpart::Config cfg;
    cfg.self_check = true;
    fox::fmpart::FMPart<SimpleHypergraph> fm(g, cfg);
    auto r = fm.run({}, fixed);
    ExpectEq("fixed v0 stays", r.part[0], 1);
    ExpectEq("fixed v7 stays", r.part[7], 0);
    ExpectEq("fixed self-check clean", r.self_check_failures, 0);
    ExpectTrue("fixed balanced", r.balanced);
    ExpectEq("fixed optimal cut", r.cut, 1);     // swapping the two clusters as wholes is enough
    ExpectEq("fixed ref agrees", RefCut(g, r.part), r.cut);
}

void TestOneSideAllFixed()
{
    // Start all on side0 (unbalanced); 0..3 fixed: free cluster {4..7} must move as a whole.
    // Also checks the balance-repair exception: first pass cum is negative (0 -> 1 cut)
    // but achieves balanced, so the driver loop must not stop here.
    SimpleHypergraph g = TwoClusters();
    const std::vector<uint8_t> init(8, 0);
    const std::vector<int8_t> fixed = {0,0,0,0,-1,-1,-1,-1};
    fox::fmpart::Config cfg;
    cfg.self_check = true;
    fox::fmpart::FMPart<SimpleHypergraph> fm(g, cfg);
    auto r = fm.run(init, fixed);
    ExpectEq("oneside self-check clean", r.self_check_failures, 0);
    ExpectTrue("oneside balanced", r.balanced);
    for (int v = 0; v < 4; ++v)
        ExpectEq("oneside fixed intact", r.part[v], 0);
    ExpectEq("oneside cut", r.cut, 1);
    ExpectEq("oneside ref agrees", RefCut(g, r.part), r.cut);
}

void TestAllFixed()
{
    SimpleHypergraph g = TwoClusters();
    const std::vector<uint8_t> init = {0,1,0,1,0,1,0,1};
    const std::vector<int8_t> fixed = {0,1,0,1,0,1,0,1};
    fox::fmpart::Config cfg;
    cfg.self_check = true;
    fox::fmpart::FMPart<SimpleHypergraph> fm(g, cfg);
    auto r = fm.run(init, fixed);
    ExpectEq("allfixed cut unchanged", r.cut, r.initial_cut);
    ExpectEq("allfixed ref agrees", RefCut(g, r.part), r.cut);
    ExpectTrue("allfixed terminates", r.passes >= 1);
    for (int v = 0; v < 8; ++v)
        ExpectEq("allfixed pinned", r.part[v], v % 2);
    ExpectEq("allfixed self-check clean", r.self_check_failures, 0);
}

void TestInfeasibleFixed()
{
    // total 13, avg 6, max_weight 7; v0 alone (weight 10) exceeds cap -> infeasible
    SimpleHypergraph g;
    g.nv = 4;
    g.pins = {{0,1},{1,2},{2,3}};
    g.vweights = {10, 1, 1, 1};
    const std::vector<uint8_t> init = {0,0,0,0};
    const std::vector<int8_t> fixed = {0,-1,-1,-1};
    fox::fmpart::Config cfg;
    cfg.self_check = true;
    fox::fmpart::FMPart<SimpleHypergraph> fm(g, cfg);
    auto r = fm.run(init, fixed);                 // no infinite loop / crash is most of the pass
    ExpectTrue("infeasible reports unbalanced", !r.balanced);
    ExpectEq("infeasible self-check clean", r.self_check_failures, 0);
    ExpectEq("infeasible ref agrees", RefCut(g, r.part), r.cut);
}

void TestWeightedNets()
{
    // Optimum {0,1}|{2,3}: only the two light nets cut (cut 2); heavy nets (5) stay intact
    SimpleHypergraph g;
    g.nv = 4;
    g.pins = {{0,1},{2,3},{0,2},{1,3}};
    g.nweights = {5, 5, 1, 1};
    const std::vector<uint8_t> init = {0,1,0,1};  // start cuts both heavy nets, cut 10
    fox::fmpart::Config cfg;
    cfg.self_check = true;
    fox::fmpart::FMPart<SimpleHypergraph> fm(g, cfg);
    auto r = fm.run(init);
    ExpectEq("weighted nets cut", r.cut, 2);
    ExpectEq("weighted nets self-check", r.self_check_failures, 0);
    ExpectEq("weighted nets ref agrees", RefCut(g, r.part), r.cut);
}

void TestWeightedVertices()
{
    // total 8, avg 4, slack 1, max_weight 5: ring {0,1,2,3} weight 6 cannot fit one side,
    // so the ring must be cut (2 edges); {4,5} stay together -> best feasible cut 2
    SimpleHypergraph g;
    g.nv = 6;
    g.pins = {{0,1},{1,2},{2,3},{0,3},{4,5}};
    g.vweights = {3,1,1,1,1,1};
    fox::fmpart::Config cfg;
    cfg.self_check = true;
    fox::fmpart::FMPart<SimpleHypergraph> fm(g, cfg);
    ExpectEq("weighted max_weight", fm.max_weight(), 5);
    auto r = fm.run();
    ExpectTrue("weighted vertices balanced", r.balanced);
    ExpectEq("weighted vertices cut", r.cut, 2);
    ExpectEq("weighted vertices self-check", r.self_check_failures, 0);
}

SimpleHypergraph RandomHypergraph(std::mt19937 &rng)
{
    std::uniform_int_distribution<int> nvd(2, 40), ned(1, 80), pind(2, 5);
    SimpleHypergraph g;
    g.nv = nvd(rng);
    const int ne = ned(rng);
    std::uniform_int_distribution<int> vd(0, g.nv - 1);
    for (int e = 0; e < ne; ++e) {
        std::set<int> s;
        const int k = std::min(pind(rng), g.nv);
        while ((int)s.size() < k)
            s.insert(vd(rng));
        g.pins.emplace_back(s.begin(), s.end());
    }
    return g;
}

void TestRandomStress()
{
    // Fixed seeds so failures are reproducible (spec §6.4)
    for (unsigned seed = 1; seed <= 20; ++seed) {
        std::mt19937 rng(seed);
        SimpleHypergraph g = RandomHypergraph(rng);
        fox::fmpart::Config cfg;
        cfg.self_check = true;
        cfg.seed = seed;
        fox::fmpart::FMPart<SimpleHypergraph> fm(g, cfg);
        auto r = fm.run();
        char label[64];
        std::snprintf(label, sizeof label, "stress seed %u self-check", seed);
        ExpectEq(label, r.self_check_failures, 0);
        std::snprintf(label, sizeof label, "stress seed %u balanced", seed);
        ExpectTrue(label, r.balanced);       // with unit weights, balance is always feasible
        std::snprintf(label, sizeof label, "stress seed %u ref cut", seed);
        ExpectEq(label, RefCut(g, r.part), r.cut);

        // Three single-pass continuations to check monotonicity
        fox::fmpart::Config c1 = cfg;
        c1.max_passes = 1;
        fox::fmpart::FMPart<SimpleHypergraph> fm1(g, c1);
        std::vector<uint8_t> cur = r.part;
        int prev = r.cut;
        for (int p = 0; p < 3; ++p) {
            auto rr = fm1.run(cur);
            std::snprintf(label, sizeof label, "stress seed %u monotonic", seed);
            ExpectTrue(label, rr.cut <= prev);
            prev = rr.cut;
            cur = rr.part;
        }
    }
}

void TestAbcWrapper()
{
    // Hand-built 4-vertex logic net: pi0,pi1 -> n0 -> n1 -> po; pi1 also fans out to n1.
    // Expected hypergraph (same construction as hpart.cpp:164):
    //   vertices = {pi0, pi1, n0, n1} (PO is not a hypergraph vertex)
    //   edges: pi0:{pi0,n0}  pi1:{pi1,n0,n1}  n0:{n0,n1}; n1's edge has 1 pin, dropped
    Abc_Ntk_t *pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    Abc_Obj_t *pi0 = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *pi1 = Abc_NtkCreatePi(pNtk);
    Abc_Obj_t *n0 = Abc_NtkCreateNode(pNtk);
    Abc_ObjAddFanin(n0, pi0);
    Abc_ObjAddFanin(n0, pi1);
    n0->pData = Abc_SopCreateAnd((Mem_Flex_t *)pNtk->pManFunc, 2, NULL);
    Abc_Obj_t *n1 = Abc_NtkCreateNode(pNtk);
    Abc_ObjAddFanin(n1, n0);
    Abc_ObjAddFanin(n1, pi1);
    n1->pData = Abc_SopCreateAnd((Mem_Flex_t *)pNtk->pManFunc, 2, NULL);
    Abc_Obj_t *po = Abc_NtkCreatePo(pNtk);
    Abc_ObjAddFanin(po, n1);

    fox::fmpart::AbcNtkWrapper g(pNtk);
    ExpectEq("wrapper vertices", g.num_vertices(), 4);
    ExpectEq("wrapper nets", g.num_nets(), 3);
    int total_pins = 0;
    for (int e = 0; e < g.num_nets(); ++e)
        total_pins += (int)g.pins_of(e).size();
    ExpectEq("wrapper pins", total_pins, 7);
    ExpectTrue("vertex_to_obj works", g.vertex_to_obj(0) != nullptr);

    fox::fmpart::Config cfg;
    cfg.self_check = true;
    fox::fmpart::FMPart<fox::fmpart::AbcNtkWrapper> fm(g, cfg);   // second instantiation (spec §6.1)
    auto r = fm.run();
    ExpectEq("wrapper fm self-check", r.self_check_failures, 0);
    ExpectTrue("wrapper fm balanced", r.balanced);
    ExpectEq("wrapper fm ref-free cut sane", r.cut >= 0 && r.cut <= 3 ? 1 : 0, 1);

    Abc_NtkDelete(pNtk);
}

// patoh reference: returns -1 if tools are not on PATH or the run fails
int RunPatohReference(const fox::fmpart::AbcNtkWrapper &g)
{
    if (std::system("command -v HgrToPaToH >/dev/null 2>&1") != 0
        || std::system("command -v patoh >/dev/null 2>&1") != 0)
        return -1;

    char tmpl[] = "/tmp/fmpart_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (dir == nullptr)
        return -1;

    int patoh_cut = -1;
    const std::string hgr = std::string(dir) + "/net.hgr";
    const std::string pat = std::string(dir) + "/net.patoh";
    {
        std::ofstream out(hgr);
        out << g.num_nets() << ' ' << g.num_vertices() << '\n';
        for (int e = 0; e < g.num_nets(); ++e) {
            const auto &pins = g.pins_of(e);
            for (std::size_t k = 0; k < pins.size(); ++k)
                out << (k ? " " : "") << pins[k] + 1;   // hgr format is 1-based
            out << '\n';
        }
    }
    const std::string cmd =
        "HgrToPaToH '" + hgr + "' '" + pat + "' >/dev/null 2>&1 && "
        "patoh '" + pat + "' 2 UM=O IB=0.02 >/dev/null 2>&1";
    const int rc = std::system(cmd.c_str());
    std::ifstream in(pat + ".part.2");
    std::vector<int> parts;
    int p;
    while (in >> p)
        parts.push_back(p);
    if (rc == 0 && (int)parts.size() == g.num_vertices()) {
        patoh_cut = 0;
        for (int e = 0; e < g.num_nets(); ++e) {
            bool s0 = false, s1 = false;
            for (int v : g.pins_of(e))
                (parts[v] ? s1 : s0) = true;
            if (s0 && s1)
                ++patoh_cut;
        }
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return patoh_cut;
}

// Real-circuit mode (spec §6.5): print FM vs patoh cuts; no asserts, not a gate
int RunCircuitFile(const char *path)
{
    Abc_Ntk_t *pNtk = Io_Read(const_cast<char *>(path),
                              Io_ReadFileType(const_cast<char *>(path)), 1, 0);
    if (pNtk == nullptr) {
        std::fprintf(stderr, "fmpart: cannot read %s\n", path);
        return 1;
    }

    fox::fmpart::AbcNtkWrapper g(pNtk);
    fox::fmpart::Config cfg;
    fox::fmpart::FMPart<fox::fmpart::AbcNtkWrapper> fm(g, cfg);
    auto r = fm.run();

    int patoh_cut = RunPatohReference(g);

    std::printf("%-16s v=%6d nets=%6d | fm cut %5d (init %5d, %2d passes, balanced=%d) | patoh cut ",
                std::filesystem::path(path).filename().c_str(),
                g.num_vertices(), g.num_nets(),
                r.cut, r.initial_cut, r.passes, (int)r.balanced);
    if (patoh_cut >= 0)
        std::printf("%5d\n", patoh_cut);
    else
        std::printf("  n/a\n");

    Abc_NtkDelete(pNtk);
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    Abc_Start();
    int ret = 0;
    if (argc > 1) {
        for (int i = 1; i < argc; ++i)
            ret |= RunCircuitFile(argv[i]);
        Abc_Stop();
        return ret;
    }
    TestBucketsBasic();
    TestBucketsFindTop();
    TestBucketsDegenerate();
    TestEchoWithZeroPasses();
    TestFixedOverridesInit();
    TestRandomInitBalanced();
    TestRunReuse();
    TestDegenerate();
    TestKnownOptimal();
    TestMonotonicPasses();
    TestFixedPins();
    TestOneSideAllFixed();
    TestAllFixed();
    TestInfeasibleFixed();
    TestWeightedNets();
    TestWeightedVertices();
    TestRandomStress();
    TestAbcWrapper();
    if (g_fail == 0) std::printf("all fmpart tests passed\n");
    const int result = g_fail == 0 ? 0 : 1;
    Abc_Stop();
    return result;
}
