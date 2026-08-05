#include <cstdint>
#include <cstdio>
#include <random>
#include <set>
#include <span>
#include <vector>

#include "base/abc/abc.h"
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

void TestFixedPins()
{
    SimpleHypergraph g = TwoClusters();
    std::vector<int8_t> fixed(8, -1);
    fixed[0] = 1;                        // 逆着自然聚类方向钉
    fixed[7] = 0;
    fox::fmpart::Config cfg;
    cfg.self_check = true;
    fox::fmpart::FMPart<SimpleHypergraph> fm(g, cfg);
    auto r = fm.run({}, fixed);
    ExpectEq("fixed v0 stays", r.part[0], 1);
    ExpectEq("fixed v7 stays", r.part[7], 0);
    ExpectEq("fixed self-check clean", r.self_check_failures, 0);
    ExpectTrue("fixed balanced", r.balanced);
    ExpectEq("fixed optimal cut", r.cut, 1);     // 两团整体换边即可
    ExpectEq("fixed ref agrees", RefCut(g, r.part), r.cut);
}

void TestOneSideAllFixed()
{
    // 起点全在 side0（不平衡），0..3 钉死：自由团 {4..7} 必须整体迁走。
    // 这条同时验证平衡修复趟例外：第一趟 cum 为负（0 -> 1 条 cut），
    // 但换来了 balanced，驱动循环不得在这里终止。
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
    // total 13, avg 6, max_weight 7；v0 (weight 10) 单独超重 -> 约束无解
    SimpleHypergraph g;
    g.nv = 4;
    g.pins = {{0,1},{1,2},{2,3}};
    g.vweights = {10, 1, 1, 1};
    const std::vector<uint8_t> init = {0,0,0,0};
    const std::vector<int8_t> fixed = {0,-1,-1,-1};
    fox::fmpart::Config cfg;
    cfg.self_check = true;
    fox::fmpart::FMPart<SimpleHypergraph> fm(g, cfg);
    auto r = fm.run(init, fixed);                 // 不死循环、不崩即通过大半
    ExpectTrue("infeasible reports unbalanced", !r.balanced);
    ExpectEq("infeasible self-check clean", r.self_check_failures, 0);
    ExpectEq("infeasible ref agrees", RefCut(g, r.part), r.cut);
}

void TestWeightedNets()
{
    // 最优 {0,1}|{2,3}：只切两条轻 net（cut 2），重 net (5) 保持完整
    SimpleHypergraph g;
    g.nv = 4;
    g.pins = {{0,1},{2,3},{0,2},{1,3}};
    g.nweights = {5, 5, 1, 1};
    const std::vector<uint8_t> init = {0,1,0,1};  // 起点切开两条重 net，cut 10
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
    // total 8, avg 4, slack 1, max_weight 5：环 {0,1,2,3} 权重 6 放不进一侧，
    // 必须切开环（2 条），{4,5} 保持一侧 -> 最优可行 cut 2
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
    // 种子写死，失败可复现（spec §6.4）
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
        ExpectTrue(label, r.balanced);       // 全 1 权重下平衡总可行
        std::snprintf(label, sizeof label, "stress seed %u ref cut", seed);
        ExpectEq(label, RefCut(g, r.part), r.cut);

        // 单趟续跑 3 次验证单调性
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
    // 手搭 4 顶点逻辑网络：pi0,pi1 -> n0 -> n1 -> po，pi1 同时扇出到 n1。
    // 期望超图（hpart.cpp:164 同口径）：
    //   vertices = {pi0, pi1, n0, n1}（PO 不是超图顶点）
    //   edges: pi0:{pi0,n0}  pi1:{pi1,n0,n1}  n0:{n0,n1}；n1 的边只剩 1 pin，弃
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
    fox::fmpart::FMPart<fox::fmpart::AbcNtkWrapper> fm(g, cfg);   // 第二个实例化（spec §6.1）
    auto r = fm.run();
    ExpectEq("wrapper fm self-check", r.self_check_failures, 0);
    ExpectTrue("wrapper fm balanced", r.balanced);
    ExpectEq("wrapper fm ref-free cut sane", r.cut >= 0 && r.cut <= 3 ? 1 : 0, 1);

    Abc_NtkDelete(pNtk);
}

} // namespace

int main()
{
    Abc_Start();
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
