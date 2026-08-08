#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

#include "base/abc/abc.h"
#include "base/io/ioAbc.h"
#include "hive/hive.hpp"

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

} // namespace

int main()
{
    Abc_Start();
    TestConfigDefaults();
    TestRunHivePreconditions();
    if (g_fail == 0) std::printf("all hive tests passed\n");
    const int result = g_fail == 0 ? 0 : 1;
    Abc_Stop();
    return result;
}
