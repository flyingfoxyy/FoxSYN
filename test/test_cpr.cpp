#include <cstdio>
#include <initializer_list>

#include "base/abc/abc.h"
#include "cpr/cpr.hpp"
#include "timer/timer.hpp"

namespace {

bool ExpectTrue(const char *label, bool actual)
{
    if (actual)
        return true;
    std::fprintf(stderr, "%s: expected true, got false\n", label);
    return false;
}

bool ExpectEqual(const char *label, int actual, int expected)
{
    if (actual == expected)
        return true;
    std::fprintf(stderr, "%s: expected %d, got %d\n", label, expected, actual);
    return false;
}

bool ExpectLess(const char *label, float lhs, float rhs)
{
    if (lhs < rhs)
        return true;
    std::fprintf(stderr, "%s: expected %.2f < %.2f\n", label, lhs, rhs);
    return false;
}

float ComputeMaxArrival(Abc_Ntk_t *pNtk)
{
    fox::timer::SimpleTimer timer(pNtk);
    timer.compute_arrival();
    return timer.max_arrival();
}

struct BasicRelocateNtk {
    Abc_Ntk_t *pNtk = nullptr;
    Abc_Obj_t *pPi = nullptr;
    Abc_Obj_t *pN1 = nullptr;
    Abc_Obj_t *pN2 = nullptr;
    Abc_Obj_t *pN3 = nullptr;
    Abc_Obj_t *pN4 = nullptr;
    Abc_Obj_t *pN5 = nullptr;
    Abc_Obj_t *pPo = nullptr;
};

struct SplitRelocateNtk {
    Abc_Ntk_t *pNtk = nullptr;
    Abc_Obj_t *pPi = nullptr;
    Abc_Obj_t *pN1 = nullptr;
    Abc_Obj_t *pN2 = nullptr;
    Abc_Obj_t *pN3 = nullptr;
    Abc_Obj_t *pN4 = nullptr;
    Abc_Obj_t *pN5 = nullptr;
    Abc_Obj_t *pN6 = nullptr;
    Abc_Obj_t *pExtra0 = nullptr;
    Abc_Obj_t *pExtra1 = nullptr;
    Abc_Obj_t *pExtra2 = nullptr;
    Abc_Obj_t *pPo = nullptr;
};

void ConnectChain(Abc_Obj_t *pPi, const std::initializer_list<Abc_Obj_t *> &nodes, Abc_Obj_t *pPo)
{
    Abc_Obj_t *prev = pPi;
    for (Abc_Obj_t *pNode : nodes)
    {
        Abc_ObjAddFanin(pNode, prev);
        prev = pNode;
    }
    Abc_ObjAddFanin(pPo, prev);
}

BasicRelocateNtk CreateBasicRelocateNtk()
{
    BasicRelocateNtk test;
    test.pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    test.pPi = Abc_NtkCreatePi(test.pNtk);
    test.pN1 = Abc_NtkCreateNode(test.pNtk);
    test.pN2 = Abc_NtkCreateNode(test.pNtk);
    test.pN3 = Abc_NtkCreateNode(test.pNtk);
    test.pN4 = Abc_NtkCreateNode(test.pNtk);
    test.pN5 = Abc_NtkCreateNode(test.pNtk);
    test.pPo = Abc_NtkCreatePo(test.pNtk);
    ConnectChain(test.pPi, {test.pN1, test.pN2, test.pN3, test.pN4, test.pN5}, test.pPo);
    return test;
}

SplitRelocateNtk CreateSplitRelocateNtk()
{
    SplitRelocateNtk test;
    test.pNtk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    test.pPi = Abc_NtkCreatePi(test.pNtk);
    test.pN1 = Abc_NtkCreateNode(test.pNtk);
    test.pN2 = Abc_NtkCreateNode(test.pNtk);
    test.pN3 = Abc_NtkCreateNode(test.pNtk);
    test.pN4 = Abc_NtkCreateNode(test.pNtk);
    test.pN5 = Abc_NtkCreateNode(test.pNtk);
    test.pN6 = Abc_NtkCreateNode(test.pNtk);
    test.pExtra0 = Abc_NtkCreateNode(test.pNtk);
    test.pExtra1 = Abc_NtkCreateNode(test.pNtk);
    test.pExtra2 = Abc_NtkCreateNode(test.pNtk);
    test.pPo = Abc_NtkCreatePo(test.pNtk);

    ConnectChain(test.pPi, {test.pN1, test.pN2, test.pN3, test.pN4, test.pN5, test.pN6}, test.pPo);
    Abc_ObjAddFanin(test.pExtra0, test.pPi);
    Abc_ObjAddFanin(test.pExtra1, test.pPi);
    Abc_ObjAddFanin(test.pExtra2, test.pPi);
    return test;
}

void AssignBasicRelocateParts(const BasicRelocateNtk &test)
{
    Abc_ObjSetPartId(test.pPi, 0);
    Abc_ObjSetPartId(test.pN1, 0);
    Abc_ObjSetPartId(test.pN2, 1);
    Abc_ObjSetPartId(test.pN3, 1);
    Abc_ObjSetPartId(test.pN4, 1);
    Abc_ObjSetPartId(test.pN5, 2);
}

void AssignSplitRelocateParts(const SplitRelocateNtk &test)
{
    Abc_ObjSetPartId(test.pPi, 0);
    Abc_ObjSetPartId(test.pN1, 0);
    Abc_ObjSetPartId(test.pN2, 1);
    Abc_ObjSetPartId(test.pN3, 1);
    Abc_ObjSetPartId(test.pN4, 1);
    Abc_ObjSetPartId(test.pN5, 1);
    Abc_ObjSetPartId(test.pN6, 2);
    Abc_ObjSetPartId(test.pExtra0, 0);
    Abc_ObjSetPartId(test.pExtra1, 2);
    Abc_ObjSetPartId(test.pExtra2, 2);
}

bool TestRelocateMovesWholeSegmentToSink()
{
    BasicRelocateNtk test = CreateBasicRelocateNtk();
    AssignBasicRelocateParts(test);

    fox::cpr::Config cfg;
    cfg.balance_pct = 99;
    cfg.cutsize_growth_pct = 0;
    cfg.relocate_max_rounds = 4;
    cfg.relocate_stall_limit = 1;
    cfg.replicate_max_rounds = 0;

    const float before = ComputeMaxArrival(test.pNtk);
    const bool ok_apply = fox::cpr::ApplyCpr(test.pNtk, cfg);
    const float after = ComputeMaxArrival(test.pNtk);

    bool ok = true;
    ok &= ExpectTrue("basic relocate apply succeeds", ok_apply);
    ok &= ExpectLess("basic relocate improves delay", after, before);
    ok &= ExpectEqual("basic segment node 2 moved to sink", Abc_ObjGetPartId(test.pN2), 2);
    ok &= ExpectEqual("basic segment node 3 moved to sink", Abc_ObjGetPartId(test.pN3), 2);
    ok &= ExpectEqual("basic segment node 4 moved to sink", Abc_ObjGetPartId(test.pN4), 2);

    Abc_NtkDelete(test.pNtk);
    return ok;
}

bool TestRelocatePrefersSplitWhenBalanceCostIsLower()
{
    SplitRelocateNtk test = CreateSplitRelocateNtk();
    AssignSplitRelocateParts(test);

    fox::cpr::Config cfg;
    cfg.balance_pct = 50;
    cfg.cutsize_growth_pct = 0;
    cfg.relocate_max_rounds = 4;
    cfg.relocate_stall_limit = 1;
    cfg.replicate_max_rounds = 0;

    const float before = ComputeMaxArrival(test.pNtk);
    const bool ok_apply = fox::cpr::ApplyCpr(test.pNtk, cfg);
    const float after = ComputeMaxArrival(test.pNtk);

    bool ok = true;
    ok &= ExpectTrue("split relocate apply succeeds", ok_apply);
    ok &= ExpectLess("split relocate improves delay", after, before);
    ok &= ExpectEqual("split front node moved to src", Abc_ObjGetPartId(test.pN2), 0);
    ok &= ExpectEqual("split front-mid node moved to src", Abc_ObjGetPartId(test.pN3), 0);
    ok &= ExpectEqual("split back-mid node moved to sink", Abc_ObjGetPartId(test.pN4), 2);
    ok &= ExpectEqual("split back node moved to sink", Abc_ObjGetPartId(test.pN5), 2);

    Abc_NtkDelete(test.pNtk);
    return ok;
}

} // namespace

int main()
{
    bool ok = true;
    ok &= TestRelocateMovesWholeSegmentToSink();
    ok &= TestRelocatePrefersSplitWhenBalanceCostIsLower();
    return ok ? 0 : 1;
}
