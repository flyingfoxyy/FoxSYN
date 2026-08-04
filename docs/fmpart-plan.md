# fmpart 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 按 `docs/fmpart-design.md` 实现模板化 FM 二分划分器：`fox::fmpart::FMPart<G>` + `GainBuckets` + `AbcNtkWrapper` + 测试驱动 `test_fmpart`。

**Architecture:** header-only 模板核心（`fm_buckets.hpp`、`fmpart.hpp`，零 ABC 依赖）；`AbcNtkWrapper` 单独成库（`fmpart`，链 `libabc`）；测试驱动无参运行跑全部断言测试，带文件参数运行进入真实电路模式（FM vs patoh 并排打印）。

**Tech Stack:** C++23，CMake，ABC（仅 wrapper 与电路模式），无测试框架（`g_fail` 计数器风格，同 `src/test_csr4.cpp`）。

## Global Constraints

- C++23（各模块 CMakeLists 均 `set(CMAKE_CXX_STANDARD 23)`）
- 4 空格缩进，类型 `PascalCase`，变量/函数 `snake_case`，命名空间 `fox::fmpart`
- **release 构建定义 `NDEBUG`，`assert` 失效**——所有测试判定必须走 `g_fail` 计数器；`assert` 只用于「调用方 bug」类前置条件（spec §3.3）
- `fmpart.hpp` 与 `fm_buckets.hpp` **不得 include 任何 ABC 头文件**（spec §8）；Task 1–4 的 `test_fmpart` 不链接 `libabc`，这本身就是验证
- 平衡上界公式与 `cpr.cpp:254` 逐字符一致：`slack = (avg * pct + 99) / 100; max_weight = max(avg + slack, avg + 1)`
- 构建：首次 `make release`，之后增量 `cmake --build release --target test_fmpart -j$(nproc)`
- 每个 Task 结束时 `./release/test_fmpart` 必须退出码 0，然后才 commit
- spec 是权威；实现中发现的两处必要偏差（`self_check` 开关、平衡修复趟的终止例外）在引入它们的 Task 里同步修订 spec 并一起提交

---

### Task 1: GainBuckets 增益桶 + 测试脚手架

**Files:**
- Create: `src/fmpart/fm_buckets.hpp`
- Create: `src/test_fmpart.cpp`
- Modify: `src/CMakeLists.txt`（新增 `test_fmpart` 目标）

**Interfaces:**
- Produces: `fox::fmpart::GainBuckets`，后续 Task 3 依赖的成员：
  - `static constexpr int kNone = -1`
  - `void reset(int num_vertices, int gmax)`
  - `void insert(int v, int side, int gain)` / `void erase(int v)` / `void update_gain(int v, int gain)`（保持 side 不变）
  - `bool contains(int v) const` / `int gain_of(int v) const` / `int side_of(int v) const`
  - `int max_gain(int side)`（懒下降后返回真实最大；空侧返回 `-gmax-1`）/ `bool empty(int side)`
  - `template <typename Pred> int find_top(int side, Pred feasible)`（从顶向下第一个满足谓词的顶点，找不到返回 `kNone`；不把 max 指针降过非空桶）
  - `int check_consistency()`（结构自检，返回不一致数，打印到 stderr）

- [ ] **Step 1: 写 `src/fmpart/fm_buckets.hpp`**

```cpp
#ifndef FMPART_FM_BUCKETS_HPP
#define FMPART_FM_BUCKETS_HPP

#include <cstdio>
#include <vector>

namespace fox::fmpart {

// FM 增益桶：每侧一个桶数组，桶内侵入式双向链表，max 指针懒下降。
// 除 find_top 的向下扫描外全部 O(1)（见 docs/fmpart-design.md §5.2.1）。
class GainBuckets {
public:
    static constexpr int kNone = -1;

    void reset(int num_vertices, int gmax)
    {
        m_gmax = gmax;
        m_width = 2 * gmax + 1;
        m_head.assign(2 * m_width, kNone);
        m_next.assign(num_vertices, kNone);
        m_prev.assign(num_vertices, kNone);
        m_gain.assign(num_vertices, 0);
        m_side.assign(num_vertices, 0);
        m_in.assign(num_vertices, 0);
        m_max[0] = m_max[1] = -m_gmax - 1;   // 低于一切合法增益 == 空
    }

    bool contains(int v) const { return m_in[v] != 0; }
    int  gain_of(int v) const { return m_gain[v]; }
    int  side_of(int v) const { return m_side[v]; }

    void insert(int v, int side, int gain)
    {
        // 调用方保证 !contains(v) 且 -gmax <= gain <= gmax
        const int b = bucket_index(side, gain);
        m_gain[v] = gain;
        m_side[v] = side;
        m_in[v] = 1;
        m_prev[v] = kNone;
        m_next[v] = m_head[b];
        if (m_head[b] != kNone)
            m_prev[m_head[b]] = v;
        m_head[b] = v;
        if (gain > m_max[side])
            m_max[side] = gain;
    }

    void erase(int v)
    {
        if (!m_in[v])
            return;
        const int b = bucket_index(m_side[v], m_gain[v]);
        if (m_prev[v] != kNone)
            m_next[m_prev[v]] = m_next[v];
        else
            m_head[b] = m_next[v];
        if (m_next[v] != kNone)
            m_prev[m_next[v]] = m_prev[v];
        m_next[v] = m_prev[v] = kNone;
        m_in[v] = 0;
        // m_max 允许暂时虚高，max_gain() 查询时懒下降
    }

    void update_gain(int v, int gain)
    {
        const int side = m_side[v];
        erase(v);
        insert(v, side, gain);
    }

    bool empty(int side) { return max_gain(side) < -m_gmax; }

    // 该侧当前最大增益；空侧返回 -gmax-1
    int max_gain(int side)
    {
        int g = m_max[side];
        while (g >= -m_gmax && m_head[bucket_index(side, g)] == kNone)
            --g;
        m_max[side] = g;
        return g;
    }

    // 从 side 顶端向下找第一个满足 feasible 的顶点，找不到返回 kNone。
    // 只经由 max_gain() 跳过空桶；不会把 max 指针降过仍有元素的桶。
    template <typename Pred>
    int find_top(int side, Pred feasible)
    {
        for (int g = max_gain(side); g >= -m_gmax; --g)
            for (int v = m_head[bucket_index(side, g)]; v != kNone; v = m_next[v])
                if (feasible(v))
                    return v;
        return kNone;
    }

    // 结构自检：返回不一致数（0 = 一致），逐条打印到 stderr
    int check_consistency()
    {
        int bad = 0;
        for (int side = 0; side < 2; ++side)
            for (int g = -m_gmax; g <= m_gmax; ++g)
                for (int v = m_head[bucket_index(side, g)]; v != kNone; v = m_next[v]) {
                    if (!m_in[v] || m_side[v] != side || m_gain[v] != g) {
                        std::fprintf(stderr, "GainBuckets: vertex %d in wrong bucket\n", v);
                        ++bad;
                    }
                    if (m_next[v] != kNone && m_prev[m_next[v]] != v) {
                        std::fprintf(stderr, "GainBuckets: broken links at %d\n", v);
                        ++bad;
                    }
                }
        for (int side = 0; side < 2; ++side) {
            int true_max = -m_gmax - 1;
            for (int g = m_gmax; g >= -m_gmax; --g)
                if (m_head[bucket_index(side, g)] != kNone) { true_max = g; break; }
            if (max_gain(side) != true_max) {
                std::fprintf(stderr, "GainBuckets: settled max != true max on side %d\n", side);
                ++bad;
            }
        }
        return bad;
    }

private:
    int bucket_index(int side, int gain) const { return side * m_width + (gain + m_gmax); }

    int m_gmax = 0;
    int m_width = 1;
    int m_max[2] = {0, 0};
    std::vector<int> m_head, m_next, m_prev, m_gain, m_side;
    std::vector<char> m_in;
};

} // namespace fox::fmpart

#endif // FMPART_FM_BUCKETS_HPP
```

- [ ] **Step 2: 写 `src/test_fmpart.cpp`（本 Task 只含桶测试）**

```cpp
#include <cstdio>
#include <vector>

#include "fmpart/fm_buckets.hpp"

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

} // namespace

int main()
{
    TestBucketsBasic();
    TestBucketsFindTop();
    TestBucketsDegenerate();
    if (g_fail == 0) std::printf("all fmpart tests passed\n");
    return g_fail == 0 ? 0 : 1;
}
```

- [ ] **Step 3: 在 `src/CMakeLists.txt` 注册 `test_fmpart`**

在 `add_executable(test_csr4 ...)` 块之后追加：

```cmake
add_executable(test_fmpart "test_fmpart.cpp")
target_include_directories(test_fmpart PRIVATE ${CMAKE_SOURCE_DIR})
```

注意：`${CMAKE_SOURCE_DIR}` 在本仓库指 `src/`（project 根在 `src/CMakeLists.txt`）。故意不链接 `libabc`。

- [ ] **Step 4: 构建并运行**

```bash
make release          # 首次全量；已有 release/ 时可直接下一条
cmake --build release --target test_fmpart -j$(nproc)
./release/test_fmpart
```

预期：输出 `all fmpart tests passed`，退出码 0。

- [ ] **Step 5: Commit**

```bash
git add src/fmpart/fm_buckets.hpp src/test_fmpart.cpp src/CMakeLists.txt
git commit -m "fmpart: add gain bucket structure with tests"
```

---

### Task 2: FMPart 核心（concept、CSR 快照、初始解；pass 循环留桩）

**Files:**
- Create: `src/fmpart/fmpart.hpp`
- Modify: `src/test_fmpart.cpp`（追加测试）
- Modify: `docs/fmpart-design.md`（spec 修订：`self_check` 开关）

**Interfaces:**
- Consumes: `fox::fmpart::GainBuckets`（Task 1 全部成员）
- Produces（Task 3/4/5 依赖）:
  - `concept fox::fmpart::FMHypergraph<G>`（spec §3.1 原文）
  - `struct Config { int balance_pct=2; int max_passes=10; int min_gain=1; unsigned seed=1; bool verbose=false; bool self_check=false; }`
  - `struct Result { std::vector<uint8_t> part; int cut; int initial_cut; int passes; bool balanced; int self_check_failures; }`
  - `template <FMHypergraph G> class FMPart { FMPart(const G&, const Config&); Result run(std::span<const uint8_t> init = {}, std::span<const int8_t> fixed = {}); int max_weight() const; }`
  - 私有桩 `int run_one_pass() { return 0; }`——**Task 3 整体替换其函数体**

- [ ] **Step 1: 先在测试里写出用法（concept 断言 + 回显测试），确认编译失败**

在 `src/test_fmpart.cpp` 的 include 区追加：

```cpp
#include <cstdint>
#include <span>

#include "fmpart/fmpart.hpp"
```

匿名 namespace 内追加：

```cpp
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
```

`main()` 中在桶测试之后追加调用：

```cpp
    TestEchoWithZeroPasses();
    TestFixedOverridesInit();
    TestRandomInitBalanced();
    TestRunReuse();
    TestDegenerate();
```

- [ ] **Step 2: 构建确认失败（`fmpart.hpp` 不存在）**

```bash
cmake --build release --target test_fmpart -j$(nproc)
```

预期：编译错误 `fmpart/fmpart.hpp: No such file or directory`。

- [ ] **Step 3: 写 `src/fmpart/fmpart.hpp`**

```cpp
#ifndef FMPART_FMPART_HPP
#define FMPART_FMPART_HPP

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <random>
#include <ranges>
#include <span>
#include <vector>

#include "fmpart/fm_buckets.hpp"

namespace fox::fmpart {

// concept 之外的契约（docs/fmpart-design.md §3.1）：
//  - 顶点 id 连续覆盖 [0, num_vertices())，net id 连续覆盖 [0, num_nets())
//  - pins_of(e) 产出合法顶点 id，内部无重复
//  - vertex_weight(v) >= 0，net_weight(e) >= 0
//  - 所有成员仅在 FMPart 构造期间被调用，需可重入
template <typename G>
concept FMHypergraph = requires(const G &g, int v, int e) {
    { g.num_vertices()   } -> std::convertible_to<int>;
    { g.num_nets()       } -> std::convertible_to<int>;
    { g.vertex_weight(v) } -> std::convertible_to<int>;
    { g.net_weight(e)    } -> std::convertible_to<int>;
    { g.pins_of(e)       } -> std::ranges::input_range;
};

struct Config {
    int      balance_pct = 2;    // 平衡松弛百分比，语义同 cpr.cpp:254
    int      max_passes  = 10;   // pass 数上限
    int      min_gain    = 1;    // 一趟收益 < min_gain 即收敛退出
    unsigned seed        = 1;    // 随机初始解种子
    bool     verbose     = false;
    bool     self_check  = false; // 仅测试用：每次移动后跑 O(pins) 不变量检查
};

struct Result {
    std::vector<uint8_t> part;        // 每个顶点所属分区，0 或 1
    int  cut         = 0;             // 最终 cut：被切开的 net 权重和
    int  initial_cut = 0;             // 优化前的 cut
    int  passes      = 0;             // 实际执行的 pass 数
    bool balanced    = false;         // 最终解是否满足平衡约束
    int  self_check_failures = 0;     // cfg.self_check 发现的不一致计数
};

template <FMHypergraph G>
class FMPart {
public:
    FMPart(const G &g, const Config &cfg)
        : m_cfg(cfg)
    {
        m_nv = static_cast<int>(g.num_vertices());
        m_ne = static_cast<int>(g.num_nets());

        m_vw.resize(m_nv);
        for (int v = 0; v < m_nv; ++v)
            m_vw[v] = static_cast<int>(g.vertex_weight(v));
        m_nw.resize(m_ne);
        for (int e = 0; e < m_ne; ++e)
            m_nw[e] = static_cast<int>(g.net_weight(e));

        // net -> 顶点 CSR（spec §4.1）
        m_pin_start.assign(m_ne + 1, 0);
        for (int e = 0; e < m_ne; ++e)
            for (auto pv : g.pins_of(e)) {
                (void)pv;
                ++m_pin_start[e + 1];
            }
        for (int e = 0; e < m_ne; ++e)
            m_pin_start[e + 1] += m_pin_start[e];
        m_pin_list.resize(m_pin_start[m_ne]);
        {
            std::vector<int> fill(m_ne, 0);
            for (int e = 0; e < m_ne; ++e)
                for (auto pv : g.pins_of(e)) {
                    const int v = static_cast<int>(pv);
                    assert(v >= 0 && v < m_nv);
                    m_pin_list[m_pin_start[e] + fill[e]++] = v;
                }
        }

        // 顶点 -> net CSR（转置）
        m_net_start.assign(m_nv + 1, 0);
        for (int idx = 0; idx < (int)m_pin_list.size(); ++idx)
            ++m_net_start[m_pin_list[idx] + 1];
        for (int v = 0; v < m_nv; ++v)
            m_net_start[v + 1] += m_net_start[v];
        m_net_list.resize(m_pin_list.size());
        {
            std::vector<int> fill(m_nv, 0);
            for (int e = 0; e < m_ne; ++e)
                for (int idx = m_pin_start[e]; idx < m_pin_start[e + 1]; ++idx) {
                    const int v = m_pin_list[idx];
                    m_net_list[m_net_start[v] + fill[v]++] = e;
                }
        }

        // 平衡上界，cpr.cpp:254 语义（spec §5.1）
        m_total_weight = 0;
        for (int v = 0; v < m_nv; ++v)
            m_total_weight += m_vw[v];
        const int avg = m_total_weight / 2;
        const int slack = (avg * m_cfg.balance_pct + 99) / 100;
        m_max_weight = std::max(avg + slack, avg + 1);

        m_min_vw = m_nv > 0 ? *std::min_element(m_vw.begin(), m_vw.end()) : 0;

        // Gmax = max_v Σ 关联 net 权重（spec §4.4）
        m_gmax = 0;
        for (int v = 0; v < m_nv; ++v) {
            int s = 0;
            for (int idx = m_net_start[v]; idx < m_net_start[v + 1]; ++idx)
                s += m_nw[m_net_list[idx]];
            m_gmax = std::max(m_gmax, s);
        }
    }

    int max_weight() const { return m_max_weight; }

    Result run(std::span<const uint8_t> init = {}, std::span<const int8_t> fixed = {})
    {
        assert(init.empty() || (int)init.size() == m_nv);
        assert(fixed.empty() || (int)fixed.size() == m_nv);

        m_part.assign(m_nv, 0);
        m_fixed.assign(m_nv, int8_t{-1});
        if (!fixed.empty())
            std::copy(fixed.begin(), fixed.end(), m_fixed.begin());

        if (!init.empty()) {
            for (int v = 0; v < m_nv; ++v)
                m_part[v] = m_fixed[v] >= 0 ? (uint8_t)m_fixed[v] : (init[v] ? 1 : 0);
        } else {
            random_init();
        }

        rebuild_counts();
        m_check_failures = 0;

        Result res;
        res.initial_cut = m_cut;

        for (int p = 1; p <= m_cfg.max_passes; ++p) {
            const bool was_balanced = is_balanced();
            const int g = run_one_pass();
            res.passes = p;
            const bool now_balanced = is_balanced();
            if (m_cfg.verbose)
                std::printf("fmpart: pass %d gain %d cut %d w0 %d w1 %d\n",
                            p, g, m_cut, m_wsum[0], m_wsum[1]);
            // 平衡修复趟（false -> true）不计入收敛判断，见 spec §5.3
            if (!(now_balanced && !was_balanced) && g < m_cfg.min_gain)
                break;
        }

        res.part = m_part;
        res.cut = m_cut;
        res.balanced = is_balanced();
        res.self_check_failures = m_check_failures;
        return res;
    }

private:
    // 固定点先落位，自由点按随机顺序贪心放到较轻一侧
    void random_init()
    {
        std::mt19937 rng(m_cfg.seed);
        std::vector<int> order;
        order.reserve(m_nv);
        int w[2] = {0, 0};
        for (int v = 0; v < m_nv; ++v) {
            if (m_fixed[v] >= 0) {
                m_part[v] = (uint8_t)m_fixed[v];
                w[m_part[v]] += m_vw[v];
            } else {
                order.push_back(v);
            }
        }
        std::shuffle(order.begin(), order.end(), rng);
        for (int v : order) {
            const int side = w[0] <= w[1] ? 0 : 1;
            m_part[v] = (uint8_t)side;
            w[side] += m_vw[v];
        }
    }

    // 从 m_part 整体重建 cnt / wsum / cut
    void rebuild_counts()
    {
        m_cnt.assign(2 * m_ne, 0);
        m_wsum[0] = m_wsum[1] = 0;
        for (int v = 0; v < m_nv; ++v)
            m_wsum[m_part[v]] += m_vw[v];
        for (int e = 0; e < m_ne; ++e)
            for (int idx = m_pin_start[e]; idx < m_pin_start[e + 1]; ++idx)
                ++m_cnt[2 * e + m_part[m_pin_list[idx]]];
        m_cut = 0;
        for (int e = 0; e < m_ne; ++e)
            if (m_cnt[2 * e] > 0 && m_cnt[2 * e + 1] > 0)
                m_cut += m_nw[e];
    }

    bool is_balanced() const
    {
        return m_wsum[0] <= m_max_weight && m_wsum[1] <= m_max_weight;
    }

    // Task 3 整体替换本函数体（FM pass：增益桶、移动、前缀回滚）
    int run_one_pass()
    {
        return 0;
    }

    // ---- 图快照（构造后只读） ----
    Config m_cfg;
    int m_nv = 0, m_ne = 0;
    std::vector<int> m_vw, m_nw;
    std::vector<int> m_pin_start, m_pin_list;
    std::vector<int> m_net_start, m_net_list;
    int m_total_weight = 0;
    int m_max_weight = 0;
    int m_min_vw = 0;
    int m_gmax = 0;

    // ---- 每次 run() 重置的 FM 状态 ----
    std::vector<uint8_t> m_part;
    std::vector<int8_t> m_fixed;
    std::vector<int> m_cnt;              // 扁平 [2*e + side]
    std::vector<char> m_locked;
    int m_wsum[2] = {0, 0};
    int m_cut = 0;
    std::vector<int> m_trail;
    GainBuckets m_buckets;
    int m_check_failures = 0;
};

} // namespace fox::fmpart

#endif // FMPART_FMPART_HPP
```

- [ ] **Step 4: 构建并运行**

```bash
cmake --build release --target test_fmpart -j$(nproc)
./release/test_fmpart
```

预期：`all fmpart tests passed`，退出码 0。（pass 桩返回 0 < min_gain，等价于只回显初始解，Step 1 的测试都不依赖优化。）

- [ ] **Step 5: 修订 spec（`self_check` 是实现引入的偏差，同步记录）**

编辑 `docs/fmpart-design.md`：

1. §3.2 Config 代码块内，`bool verbose = false;` 之后加一行：

```cpp
    bool     self_check  = false; // 仅测试用：每次移动后跑 O(pins) 不变量检查
```

2. §3.2 Result 代码块内，`bool balanced = false;` 行后加：

```cpp
    int  self_check_failures = 0;     // cfg.self_check 发现的不一致计数
```

3. §6.2 首句「实现一个 `verify_invariants()`，在每趟 pass 结束后调用：」替换为：

> `verify_invariants()` 内建于 `FMPart`，`Config::self_check` 开启时自动调用：`gain[]` 与桶结构检查在**每次移动后、回滚前**执行（回滚后 gain 已失效，spec §5.3）；`cnt`/`cut`/`wsum` 检查在回滚后再执行一次。测试通过 `Result::self_check_failures == 0` 断言。

- [ ] **Step 6: Commit**

```bash
git add src/fmpart/fmpart.hpp src/test_fmpart.cpp docs/fmpart-design.md
git commit -m "fmpart: add FMPart core with CSR snapshot and run scaffolding

The pass loop is a stub returning 0 for now. Adds the self_check knob
to Config (spec amended): release builds define NDEBUG, so invariant
checks must run through explicit counters instead of assert."
```

---

### Task 3: FM pass 循环（增益、移动、前缀回滚、自检）

**Files:**
- Modify: `src/fmpart/fmpart.hpp`（替换 `run_one_pass` 桩，新增 5 个私有函数）
- Modify: `src/test_fmpart.cpp`（追加测试）
- Modify: `docs/fmpart-design.md`（spec 修订：驱动循环的平衡修复例外）

**Interfaces:**
- Consumes: Task 1 的 `GainBuckets` 全部成员；Task 2 的全部状态数组
- Produces: `run_one_pass()` 真实实现；私有函数 `compute_gain(int v)`, `pick_from(int side)`, `move_vertex(int v)`, `undo_move(int v)`, `verify_invariants(bool check_gain)`。对外行为：`run()` 开始真正优化

- [ ] **Step 1: 追加会失败的测试**

匿名 namespace 内追加：

```cpp
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
```

`main()` 追加：

```cpp
    TestKnownOptimal();
    TestMonotonicPasses();
```

- [ ] **Step 2: 构建运行，确认失败**

```bash
cmake --build release --target test_fmpart -j$(nproc) && ./release/test_fmpart
```

预期：编译通过，`FAIL optimal cut: expected 1, got 9` 等，退出码 1（桩不做任何移动）。

- [ ] **Step 3: 实现 pass。在 `fmpart.hpp` 中把 `run_one_pass` 桩整体替换为以下 6 个函数**

```cpp
    // 按 spec §4.3 定义从头计算 v 的增益
    int compute_gain(int v) const
    {
        const int F = m_part[v], T = 1 - F;
        int gain = 0;
        for (int idx = m_net_start[v]; idx < m_net_start[v + 1]; ++idx) {
            const int e = m_net_list[idx];
            if (m_cnt[2 * e + F] == 1) gain += m_nw[e];
            if (m_cnt[2 * e + T] == 0) gain -= m_nw[e];
        }
        return gain;
    }

    // side 侧桶顶向下第一个可移入对侧的顶点；min_vw 早退见 spec §5.2
    int pick_from(int side)
    {
        const int T = 1 - side;
        if (m_wsum[T] + m_min_vw > m_max_weight)
            return GainBuckets::kNone;
        const int cap = m_max_weight - m_wsum[T];
        return m_buckets.find_top(side, [&](int v) { return m_vw[v] <= cap; });
    }

    // 移动 v 到对侧：锁定、cnt/wsum/cut 增量、邻居增益两趟式更新（spec §5.4）。
    // 必须分两趟：「移动前」分支要求 m_part[v] 仍在 F，「移动后」分支要求已在 T。
    void move_vertex(int v)
    {
        const int F = m_part[v], T = 1 - F;
        m_locked[v] = 1;
        m_buckets.erase(v);

        // 第一趟：读 pre-move 计数；v 仍在 F 侧
        for (int idx = m_net_start[v]; idx < m_net_start[v + 1]; ++idx) {
            const int e = m_net_list[idx];
            const int cF = m_cnt[2 * e + F], cT = m_cnt[2 * e + T];
            if (cT == 0) {
                if (cF > 1)
                    m_cut += m_nw[e];                 // net 变为被切
                for (int j = m_pin_start[e]; j < m_pin_start[e + 1]; ++j) {
                    const int u = m_pin_list[j];
                    if (!m_locked[u])
                        m_buckets.update_gain(u, m_buckets.gain_of(u) + m_nw[e]);
                }
            } else if (cT == 1) {
                for (int j = m_pin_start[e]; j < m_pin_start[e + 1]; ++j) {
                    const int u = m_pin_list[j];
                    if (m_part[u] == T) {             // T 侧唯一 pin，必非 v
                        if (!m_locked[u])
                            m_buckets.update_gain(u, m_buckets.gain_of(u) - m_nw[e]);
                        break;
                    }
                }
            }
            if (cT > 0 && cF == 1)
                m_cut -= m_nw[e];                     // net 变为不切
            m_cnt[2 * e + F] -= 1;
            m_cnt[2 * e + T] += 1;
        }

        m_part[v] = (uint8_t)T;
        m_wsum[F] -= m_vw[v];
        m_wsum[T] += m_vw[v];

        // 第二趟：读 post-move 计数；v 已在 T 侧
        for (int idx = m_net_start[v]; idx < m_net_start[v + 1]; ++idx) {
            const int e = m_net_list[idx];
            const int cF = m_cnt[2 * e + F];
            if (cF == 0) {
                for (int j = m_pin_start[e]; j < m_pin_start[e + 1]; ++j) {
                    const int u = m_pin_list[j];
                    if (!m_locked[u])
                        m_buckets.update_gain(u, m_buckets.gain_of(u) - m_nw[e]);
                }
            } else if (cF == 1) {
                for (int j = m_pin_start[e]; j < m_pin_start[e + 1]; ++j) {
                    const int u = m_pin_list[j];
                    if (m_part[u] == F) {             // F 侧唯一 pin，必非 v
                        if (!m_locked[u])
                            m_buckets.update_gain(u, m_buckets.gain_of(u) + m_nw[e]);
                        break;
                    }
                }
            }
        }
    }

    // 回滚一次移动：只恢复 part/cnt/wsum/cut，不碰 gain（下趟整体重算，spec §5.3）
    void undo_move(int v)
    {
        const int T = m_part[v], F = 1 - T;   // v 现在 T 侧，送回 F 侧
        for (int idx = m_net_start[v]; idx < m_net_start[v + 1]; ++idx) {
            const int e = m_net_list[idx];
            const int cF = m_cnt[2 * e + F], cT = m_cnt[2 * e + T];
            if (cF == 0 && cT > 1)
                m_cut += m_nw[e];
            if (cF > 0 && cT == 1)
                m_cut -= m_nw[e];
            m_cnt[2 * e + T] -= 1;
            m_cnt[2 * e + F] += 1;
        }
        m_part[v] = (uint8_t)F;
        m_wsum[T] -= m_vw[v];
        m_wsum[F] += m_vw[v];
    }

    // 不变量自检：返回不一致数（0 = 干净），逐条打印到 stderr。
    // check_gain 仅在移动后、回滚前为真（此时增量 gain / 桶才有定义）。
    int verify_invariants(bool check_gain)
    {
        int bad = 0;
        std::vector<int> cnt(2 * m_ne, 0);
        for (int e = 0; e < m_ne; ++e)
            for (int idx = m_pin_start[e]; idx < m_pin_start[e + 1]; ++idx)
                ++cnt[2 * e + m_part[m_pin_list[idx]]];
        for (int i = 0; i < 2 * m_ne; ++i)
            if (cnt[i] != m_cnt[i]) {
                std::fprintf(stderr, "fmpart: cnt mismatch at %d\n", i);
                ++bad;
                break;
            }
        int cut = 0;
        for (int e = 0; e < m_ne; ++e)
            if (cnt[2 * e] > 0 && cnt[2 * e + 1] > 0)
                cut += m_nw[e];
        if (cut != m_cut) {
            std::fprintf(stderr, "fmpart: cut %d != tracked %d\n", cut, m_cut);
            ++bad;
        }
        int w[2] = {0, 0};
        for (int v = 0; v < m_nv; ++v)
            w[m_part[v]] += m_vw[v];
        if (w[0] != m_wsum[0] || w[1] != m_wsum[1]) {
            std::fprintf(stderr, "fmpart: wsum mismatch\n");
            ++bad;
        }
        if (check_gain) {
            for (int v = 0; v < m_nv; ++v) {
                if (m_locked[v]) {
                    if (m_buckets.contains(v)) {
                        std::fprintf(stderr, "fmpart: locked vertex %d in bucket\n", v);
                        ++bad;
                    }
                    continue;
                }
                if (!m_buckets.contains(v)) {
                    std::fprintf(stderr, "fmpart: free vertex %d missing from bucket\n", v);
                    ++bad;
                    continue;
                }
                const int gexp = compute_gain(v);
                if (m_buckets.gain_of(v) != gexp || m_buckets.side_of(v) != m_part[v]) {
                    std::fprintf(stderr, "fmpart: gain/side mismatch at %d (have %d/%d want %d/%d)\n",
                                 v, m_buckets.gain_of(v), m_buckets.side_of(v), gexp, (int)m_part[v]);
                    ++bad;
                }
            }
            bad += m_buckets.check_consistency();
        }
        return bad;
    }

    // 一趟 FM pass（spec §5.3），返回被采纳前缀的累积增益
    int run_one_pass()
    {
        m_locked.assign(m_nv, 0);
        for (int v = 0; v < m_nv; ++v)
            if (m_fixed[v] >= 0)
                m_locked[v] = 1;
        m_buckets.reset(m_nv, m_gmax);
        for (int v = 0; v < m_nv; ++v)
            if (!m_locked[v])
                m_buckets.insert(v, m_part[v], compute_gain(v));

        m_trail.clear();
        int cum = 0;
        int best_prefix = 0;
        int best_cum = 0;
        bool best_balanced = is_balanced();   // 前缀键 (balanced, cum) 字典序

        for (;;) {
            int side;
            bool forced = false;
            if (m_wsum[0] > m_max_weight) {
                side = 0;
                forced = true;
            } else if (m_wsum[1] > m_max_weight) {
                side = 1;
                forced = true;
            } else {
                if (m_buckets.empty(0) && m_buckets.empty(1))
                    break;
                if (m_buckets.empty(1))
                    side = 0;
                else if (m_buckets.empty(0))
                    side = 1;
                else {
                    const int g0 = m_buckets.max_gain(0), g1 = m_buckets.max_gain(1);
                    if (g0 != g1)
                        side = g0 > g1 ? 0 : 1;
                    else
                        side = m_wsum[0] >= m_wsum[1] ? 0 : 1;
                }
            }

            int chosen = pick_from(side);
            if (chosen == GainBuckets::kNone) {
                if (forced)
                    break;
                chosen = pick_from(1 - side);
                if (chosen == GainBuckets::kNone)
                    break;
            }

            cum += m_buckets.gain_of(chosen);
            move_vertex(chosen);
            m_trail.push_back(chosen);

            const bool bal = is_balanced();
            if ((bal && !best_balanced) || (bal == best_balanced && cum > best_cum)) {
                best_balanced = bal;
                best_cum = cum;
                best_prefix = (int)m_trail.size();
            }

            if (m_cfg.self_check)
                m_check_failures += verify_invariants(true);
        }

        for (int i = (int)m_trail.size() - 1; i >= best_prefix; --i)
            undo_move(m_trail[i]);

        if (m_cfg.self_check)
            m_check_failures += verify_invariants(false);

        return best_cum;
    }
```

- [ ] **Step 4: 构建运行**

```bash
cmake --build release --target test_fmpart -j$(nproc) && ./release/test_fmpart
```

预期：`all fmpart tests passed`，退出码 0。若 `optimal cut` 得 2 而非 1，**先怀疑 §5.4 四分支之一漏更/错更**（self-check 会先报 gain mismatch），用 systematic-debugging 而不是放宽断言。

- [ ] **Step 5: 修订 spec 驱动循环（平衡修复趟的终止例外）**

`docs/fmpart-design.md` §5.3「**驱动循环**」代码块整体替换为：

```
initial_cut = compute_cut()
for p in 1..max_passes:
    was_balanced = is_balanced()
    g = run_one_pass()
    passes = p
    now_balanced = is_balanced()
    // 平衡修复趟（false -> true）可能返回负收益，不据此终止；
    // 否则起点不平衡时第一趟就会被 g < min_gain 打断，永远没有机会优化
    if !(now_balanced && !was_balanced) 且 g < min_gain: break
cut = compute_cut()
balanced = is_balanced()
```

- [ ] **Step 6: Commit**

```bash
git add src/fmpart/fmpart.hpp src/test_fmpart.cpp docs/fmpart-design.md
git commit -m "fmpart: implement FM pass with prefix rollback and self-check

Two-sweep neighbor gain update (pre-move branches see v on the old
side, post-move branches on the new side), lexicographic
(balanced, cum) prefix key, and a spec amendment: a balance-repairing
pass with negative gain must not trigger min_gain termination."
```

---

### Task 4: FixNode / 平衡 / 加权 / 随机压测

**Files:**
- Modify: `src/test_fmpart.cpp`（纯追加测试；若测出实现 bug 则修 `fmpart.hpp`）

**Interfaces:**
- Consumes: Task 2/3 的完整 `FMPart` 公开接口；`SimpleHypergraph`、`RefCut`、`TwoClusters`

- [ ] **Step 1: 追加约束测试**

```cpp
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
```

- [ ] **Step 2: 追加随机压测**

include 区确认有 `#include <random>` 与 `#include <set>`，然后：

```cpp
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
```

`main()` 追加调用（放在 Task 3 的测试之后）：

```cpp
    TestFixedPins();
    TestOneSideAllFixed();
    TestAllFixed();
    TestInfeasibleFixed();
    TestWeightedNets();
    TestWeightedVertices();
    TestRandomStress();
```

- [ ] **Step 3: 构建运行**

```bash
cmake --build release --target test_fmpart -j$(nproc) && ./release/test_fmpart
```

预期：`all fmpart tests passed`，退出码 0。任何 FAIL 都按 systematic-debugging 处理：self-check 输出会指认第一处不一致的数组。

- [ ] **Step 4: Commit**

```bash
git add src/test_fmpart.cpp
git commit -m "fmpart: cover FixNode, balance and weighted cases with stress tests"
```

（若 Step 3 发现并修复了 `fmpart.hpp` 的 bug，把它一并 add，并在消息里说明修了什么。）

---

### Task 5: AbcNtkWrapper 适配器

**Files:**
- Create: `src/fmpart/abc_wrapper.hpp`
- Create: `src/fmpart/abc_wrapper.cpp`
- Create: `src/fmpart/CMakeLists.txt`
- Modify: `src/CMakeLists.txt`（`add_subdirectory(fmpart)`；`test_fmpart` 链接 `fmpart` 与 `libabc`）
- Modify: `src/test_fmpart.cpp`（追加 wrapper 测试 + `Abc_Start/Stop`）

**Interfaces:**
- Consumes: `FMHypergraph` concept（wrapper 必须满足它）；ABC 的 `Abc_Ntk_t`/`Abc_Obj_t`
- Produces（Task 6 及未来调用方依赖）:
  - `class fox::fmpart::AbcNtkWrapper { explicit AbcNtkWrapper(Abc_Ntk_t *pNtk); int num_vertices() const; int num_nets() const; int vertex_weight(int) const; int net_weight(int) const; const std::vector<int> &pins_of(int e) const; Abc_Obj_t *vertex_to_obj(int v) const; }`
  - CMake 库目标 `fmpart`

- [ ] **Step 1: 追加会编译失败的 wrapper 测试**

`src/test_fmpart.cpp` include 区追加：

```cpp
#include "base/abc/abc.h"
#include "fmpart/abc_wrapper.hpp"
```

匿名 namespace 内追加：

```cpp
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
```

`main()` 改为带 `Abc_Start/Abc_Stop`（最终形态，circuit 模式 Task 6 再加）：

```cpp
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
```

- [ ] **Step 2: 写 `src/fmpart/abc_wrapper.hpp`**

```cpp
#ifndef FMPART_ABC_WRAPPER_HPP
#define FMPART_ABC_WRAPPER_HPP

#include <vector>

#include "base/abc/abc.h"

namespace fox::fmpart {

// Abc_Ntk_t 的超图视图，建图口径与 hpart 的 BuildHypergraph 一致
// （hpart.cpp:164，已知重复见 docs/fmpart-design.md §7）：
// 顶点 = PI / node / latch / const1；每个 driver 一条超边，
// 覆盖其传递可达的 sink；不足 2 pin 的边丢弃。
// 快照语义：构造时建一次，之后网络的修改不反映进来。
class AbcNtkWrapper {
public:
    explicit AbcNtkWrapper(Abc_Ntk_t *pNtk);

    int num_vertices() const { return (int)m_vertices.size(); }
    int num_nets() const { return (int)m_pins.size(); }
    int vertex_weight(int) const { return 1; }
    int net_weight(int) const { return 1; }
    const std::vector<int> &pins_of(int e) const { return m_pins[e]; }

    // 结果回写（如写 Pdb）由调用方经此映射完成，本模块不写回
    Abc_Obj_t *vertex_to_obj(int v) const { return m_vertices[v]; }

private:
    std::vector<Abc_Obj_t *> m_vertices;
    std::vector<std::vector<int>> m_pins;
};

} // namespace fox::fmpart

#endif // FMPART_ABC_WRAPPER_HPP
```

- [ ] **Step 3: 写 `src/fmpart/abc_wrapper.cpp`**

```cpp
#include "fmpart/abc_wrapper.hpp"

#include <algorithm>

namespace fox::fmpart {

namespace {

// 与 hpart.cpp 的 IsHyperNode 相同（那边的 IsCarrierNode 是它的重复体，此处合一）
bool IsHyperNode(Abc_Obj_t *pObj)
{
    return pObj != nullptr
        && (Abc_ObjIsPi(pObj)
         || Abc_ObjIsNode(pObj)
         || Abc_ObjIsLatch(pObj)
         || Abc_ObjType(pObj) == ABC_OBJ_CONST1);
}

bool ShouldTraverseInterconnect(Abc_Obj_t *pObj)
{
    return pObj != nullptr
        && (Abc_ObjIsNet(pObj) || Abc_ObjIsBi(pObj) || Abc_ObjIsBo(pObj));
}

void CollectSinks(Abc_Obj_t *pObj, const std::vector<int> &obj_to_vertex,
                  std::vector<int> &sinks, std::vector<char> &visited)
{
    Abc_Obj_t *pObjR = Abc_ObjRegular(pObj);
    Abc_Obj_t *pFanout;
    int i;

    if (pObjR == nullptr || pObjR->Id < 0 || pObjR->Id >= (int)visited.size())
        return;
    if (visited[pObjR->Id])
        return;
    visited[pObjR->Id] = 1;

    if (IsHyperNode(pObjR)) {
        const int vertex_id = obj_to_vertex[pObjR->Id];
        if (vertex_id >= 0)
            sinks.push_back(vertex_id);       // 0 基；hpart 为 hmetis 格式用 1 基
        return;
    }
    if (!ShouldTraverseInterconnect(pObjR))
        return;
    Abc_ObjForEachFanout(pObjR, pFanout, i)
        CollectSinks(pFanout, obj_to_vertex, sinks, visited);
}

} // namespace

AbcNtkWrapper::AbcNtkWrapper(Abc_Ntk_t *pNtk)
{
    Abc_Obj_t *pObj;
    int i;
    std::vector<int> obj_to_vertex(Abc_NtkObjNumMax(pNtk), -1);

    Abc_NtkForEachObj(pNtk, pObj, i) {
        if (IsHyperNode(pObj)) {
            obj_to_vertex[pObj->Id] = (int)m_vertices.size();
            m_vertices.push_back(pObj);
        }
    }

    Abc_NtkForEachObj(pNtk, pObj, i) {
        if (!IsHyperNode(pObj))
            continue;
        const int carrier_vertex = obj_to_vertex[pObj->Id];
        if (carrier_vertex < 0)
            continue;

        std::vector<int> pins;
        std::vector<char> visited(Abc_NtkObjNumMax(pNtk), 0);
        pins.push_back(carrier_vertex);

        Abc_Obj_t *pCarrier = pObj;
        if (Abc_ObjIsLatch(pObj)) {
            if (Abc_ObjFanoutNum(pObj) == 0)
                continue;
            pCarrier = Abc_ObjFanout0(pObj);
        }

        Abc_Obj_t *pFanout;
        int j;
        Abc_ObjForEachFanout(pCarrier, pFanout, j)
            CollectSinks(pFanout, obj_to_vertex, pins, visited);

        std::sort(pins.begin(), pins.end());
        pins.erase(std::unique(pins.begin(), pins.end()), pins.end());
        if (pins.size() >= 2)
            m_pins.push_back(std::move(pins));
    }
}

} // namespace fox::fmpart
```

- [ ] **Step 4: 写 `src/fmpart/CMakeLists.txt`（照 csr4 样式）**

```cmake
add_library(fmpart abc_wrapper.cpp)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-fexceptions)

target_link_libraries(fmpart PRIVATE libabc)
target_include_directories(fmpart PUBLIC ${CMAKE_SOURCE_DIR}/abc/src ${CMAKE_SOURCE_DIR})
```

- [ ] **Step 5: 修改 `src/CMakeLists.txt`**

1. `add_subdirectory(csr4)` 之后加一行：

```cmake
add_subdirectory(fmpart)
```

2. `test_fmpart` 的两行改为：

```cmake
add_executable(test_fmpart "test_fmpart.cpp")
target_link_libraries(test_fmpart PRIVATE fmpart libabc)
```

（`fmpart` 的 PUBLIC include 目录同时提供 `${CMAKE_SOURCE_DIR}` 与 abc 头文件路径，原 `target_include_directories` 行删除。）

- [ ] **Step 6: 构建运行**

```bash
cmake --build release --target test_fmpart -j$(nproc) && ./release/test_fmpart
```

预期：`all fmpart tests passed`，退出码 0。若 `wrapper vertices` 得 5 而非 4：检查是否网络里出现了 const1 对象——把断言改成与实际一致前，先用 `Abc_NtkForEachObj` 打印对象类型核实，不许盲改期望值。

- [ ] **Step 7: Commit**

```bash
git add src/fmpart/abc_wrapper.hpp src/fmpart/abc_wrapper.cpp src/fmpart/CMakeLists.txt src/CMakeLists.txt src/test_fmpart.cpp
git commit -m "fmpart: add AbcNtkWrapper adapter

Same hypergraph construction as hpart's BuildHypergraph (known
duplication, recorded in fmpart-design §7). Proves the second
instantiation FMPart<AbcNtkWrapper> compiles and runs."
```

---

### Task 6: 真实电路模式（FM vs patoh 并排）+ FoxSYN 集成

**Files:**
- Modify: `src/test_fmpart.cpp`（argv 模式）
- Modify: `src/CMakeLists.txt`（FoxSYN 链接 `fmpart`，spec §8）

**Interfaces:**
- Consumes: `AbcNtkWrapper`、`FMPart`、ABC 的 `Io_Read`/`Io_ReadFileType`
- Produces: `./release/test_fmpart <file.v> [...]` 电路模式；无参行为不变

- [ ] **Step 1: 确认 `Io_Read` 签名**

```bash
grep -n "extern Abc_Ntk_t \* Io_Read" src/abc/src/base/io/ioAbc.h
```

预期形如 `Io_Read( char * pFileName, Io_FileType_t FileType, int fCheck, int fBarBufs )`（4 参）。若是 3 参（无 `fBarBufs`），下面调用去掉最后的 `0`。

- [ ] **Step 2: 在 `src/test_fmpart.cpp` 加电路模式**

include 区追加：

```cpp
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "base/io/ioAbc.h"
```

匿名 namespace 内追加：

```cpp
// 真实电路模式（spec §6.5）：打印 FM 与 patoh 的 cut，不设断言、不算门禁
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

    int patoh_cut = -1;
    if (std::system("command -v HgrToPaToH >/dev/null 2>&1") == 0
        && std::system("command -v patoh >/dev/null 2>&1") == 0) {
        char tmpl[] = "/tmp/fmpart_XXXXXX";
        char *dir = mkdtemp(tmpl);
        if (dir != nullptr) {
            const std::string hgr = std::string(dir) + "/net.hgr";
            const std::string pat = std::string(dir) + "/net.patoh";
            {
                std::ofstream out(hgr);
                out << g.num_nets() << ' ' << g.num_vertices() << '\n';
                for (int e = 0; e < g.num_nets(); ++e) {
                    const auto &pins = g.pins_of(e);
                    for (std::size_t k = 0; k < pins.size(); ++k)
                        out << (k ? " " : "") << pins[k] + 1;   // hgr 格式 1 基
                    out << '\n';
                }
            }
            const std::string cmd =
                "HgrToPaToH '" + hgr + "' '" + pat + "' >/dev/null 2>&1 && "
                "patoh '" + pat + "' 2 UM=O IB=0.02 >/dev/null 2>&1";
            std::system(cmd.c_str());
            std::ifstream in(pat + ".part.2");
            std::vector<int> parts;
            int p;
            while (in >> p)
                parts.push_back(p);
            if ((int)parts.size() == g.num_vertices()) {
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
        }
    }

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
```

`main()` 改为：

```cpp
int main(int argc, char **argv)
{
    Abc_Start();
    int ret = 0;
    if (argc > 1) {
        for (int i = 1; i < argc; ++i)
            ret |= RunCircuitFile(argv[i]);
    } else {
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
        ret = g_fail == 0 ? 0 : 1;
    }
    Abc_Stop();
    return ret;
}
```

- [ ] **Step 3: FoxSYN 链接 `fmpart`（spec §8 预留）**

`src/CMakeLists.txt` 的 `target_link_libraries(FoxSYN PRIVATE ... )` 列表中，`hpart` 之后加一行 `fmpart`。

- [ ] **Step 4: 全量构建 + 两种模式验证**

```bash
make release
./release/test_fmpart
./release/test_fmpart regression/SimpleCircuits/mcnc/alu4.v regression/SimpleCircuits/mcnc/C880.v regression/SimpleCircuits/mcnc/C3540.v
```

预期：
1. 全量 `make release` 成功（FoxSYN 链接 fmpart 无未定义符号）
2. 无参运行 `all fmpart tests passed`，退出码 0
3. 电路模式每个文件打印一行，`fm cut` 为正数、`balanced=1`、`patoh cut` 有数值（patoh 在 PATH：`/home/longfei/HyperPar-main/tools/`）。FM 的 cut 高于 patoh 属正常（单层 vs 多级）；FM cut 低于 `v` 的 1% 或高得离谱（≥ nets 的一半）则检查建图

- [ ] **Step 5: Commit**

```bash
git add src/test_fmpart.cpp src/CMakeLists.txt
git commit -m "fmpart: add circuit mode with patoh reference and link into FoxSYN

test_fmpart <file.v> reads a netlist, runs 2-way FM through
AbcNtkWrapper and prints the cut side by side with patoh's (n/a when
the tools are absent). Print-only, not a regression gate (spec 6.5)."
```

---

## 完成判据（对照 spec 的验收清单）

- [ ] `FMPart<SimpleHypergraph>` 与 `FMPart<AbcNtkWrapper>` 同时编译通过（§6.1）
- [ ] concept 5 项 + 契约注释，`fmpart.hpp`/`fm_buckets.hpp` 无 ABC include（§3.1/§8）
- [ ] `run(init, fixed)` 支持空/非空、fixed 覆盖 init（§3.3）
- [ ] 平衡上界公式与 cpr 一致，`max_weight()` 可查（§5.1）
- [ ] self-check 覆盖 cnt/cut/wsum/gain/桶五项（§6.2）
- [ ] 功能测试 6 组 + 随机压测 20 种子全绿（§6.3/§6.4）
- [ ] 电路模式在 mcnc 上打印 FM vs patoh（§6.5）
- [ ] `src/hpart/` 无任何改动（§7）
- [ ] spec 两处修订（self_check、平衡修复例外）已提交
