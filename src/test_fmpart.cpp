#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

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
    // 两个增益 5 的都不可行 -> 落到增益 2
    int v = b.find_top(0, [](int u) { return u == 2; });
    ExpectEq("find_top skips infeasible", v, 2);
    ExpectEq("max pointer intact", b.max_gain(0), 5);   // 不许降过非空桶
    v = b.find_top(0, [](int) { return false; });
    ExpectEq("find_top none", v, (long)fox::fmpart::GainBuckets::kNone);
    ExpectEq("consistency2", b.check_consistency(), 0);
}

void TestBucketsDegenerate()
{
    fox::fmpart::GainBuckets b;
    b.reset(0, 0);                       // 空图
    ExpectTrue("empty graph buckets", b.empty(0) && b.empty(1));
    b.reset(2, 0);                       // gmax 0：唯一合法增益是 0
    b.insert(0, 0, 0);
    ExpectEq("gmax0 max", b.max_gain(0), 0);
    ExpectEq("gmax0 consistency", b.check_consistency(), 0);
}

// 第二个实例化用的最小图类型（spec §6.1）
struct SimpleHypergraph {
    int nv = 0;
    std::vector<std::vector<int>> pins;      // 每条 net 的顶点列表，无重复
    std::vector<int> vweights;               // 空 = 全 1
    std::vector<int> nweights;               // 空 = 全 1

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

// 测试侧独立重算 cut，作为增量值的对照（spec §6.2）
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

// 8 顶点两团 + 一条桥；最优 2-way cut = 1（spec §6.3.1）
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
        SimpleHypergraph g;                          // 空图
        fox::fmpart::FMPart<SimpleHypergraph> fm(g, cfg);
        auto r = fm.run();
        ExpectEq("empty cut", r.cut, 0);
        ExpectTrue("empty part", r.part.empty());
    }
    {
        SimpleHypergraph g;                          // 单顶点
        g.nv = 1;
        fox::fmpart::FMPart<SimpleHypergraph> fm(g, cfg);
        auto r = fm.run();
        ExpectEq("single cut", r.cut, 0);
        ExpectTrue("single balanced", r.balanced);
    }
    {
        SimpleHypergraph g;                          // 全部 1-pin net
        g.nv = 3;
        g.pins = {{0},{1},{2}};
        fox::fmpart::FMPart<SimpleHypergraph> fm(g, cfg);
        auto r = fm.run();
        ExpectEq("1-pin nets never cut", r.cut, 0);
    }
    {
        SimpleHypergraph g;                          // 无 net
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
    const std::vector<uint8_t> init = {0,1,0,1,0,1,0,1};   // 交错起点，cut = 9
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
    // 逐趟观察：max_passes=1 反复 run，把上一轮 part 作为下一轮 init，
    // 语义上等价于连续的 pass（spec §6.3.5）
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

} // namespace

int main()
{
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
    if (g_fail == 0) std::printf("all fmpart tests passed\n");
    return g_fail == 0 ? 0 : 1;
}
