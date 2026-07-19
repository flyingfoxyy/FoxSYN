#include <cstdio>
#include "csr3/csr3.hpp"
#include "csr3/csr3_internal.hpp"

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

} // namespace

int main()
{
    TestCeilLog2();
    if (g_fail == 0) std::printf("all csr3 tests passed\n");
    return g_fail == 0 ? 0 : 1;
}
