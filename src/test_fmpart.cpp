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
