#include <cstdio>

#include "base/abc/abc.h"

namespace {

bool ExpectEqual( const char *label, int actual, int expected )
{
    if ( actual == expected )
        return true;
    std::fprintf( stderr, "%s: expected %d, got %d\n", label, expected, actual );
    return false;
}

bool ExpectTrue( const char *label, bool actual )
{
    if ( actual )
        return true;
    std::fprintf( stderr, "%s: expected true, got false\n", label );
    return false;
}

bool ExpectFalse( const char *label, bool actual )
{
    if ( !actual )
        return true;
    std::fprintf( stderr, "%s: expected false, got true\n", label );
    return false;
}

struct HopTestNtk
{
    Abc_Ntk_t * pNtk = nullptr;
    Abc_Obj_t * pA = nullptr;
    Abc_Obj_t * pB = nullptr;
    Abc_Obj_t * pC = nullptr;
    Abc_Obj_t * pN1 = nullptr;
    Abc_Obj_t * pN2 = nullptr;
    Abc_Obj_t * pN3 = nullptr;
    Abc_Obj_t * pPo = nullptr;
};

HopTestNtk CreateHopTestNtk()
{
    HopTestNtk Test;
    Test.pNtk = Abc_NtkAlloc( ABC_NTK_LOGIC, ABC_FUNC_SOP, 1 );
    Test.pA = Abc_NtkCreatePi( Test.pNtk );
    Test.pB = Abc_NtkCreatePi( Test.pNtk );
    Test.pC = Abc_NtkCreatePi( Test.pNtk );
    Test.pN1 = Abc_NtkCreateNode( Test.pNtk );
    Test.pN2 = Abc_NtkCreateNode( Test.pNtk );
    Test.pN3 = Abc_NtkCreateNode( Test.pNtk );
    Test.pPo = Abc_NtkCreatePo( Test.pNtk );

    Abc_ObjAddFanin( Test.pN1, Test.pA );
    Abc_ObjAddFanin( Test.pN1, Test.pB );
    Abc_ObjAddFanin( Test.pN2, Test.pN1 );
    Abc_ObjAddFanin( Test.pN2, Test.pC );
    Abc_ObjAddFanin( Test.pN3, Test.pN2 );
    Abc_ObjAddFanin( Test.pN3, Test.pB );
    Abc_ObjAddFanin( Test.pPo, Test.pN3 );
    return Test;
}

void AssignHopTestParts( const HopTestNtk & Test )
{
    Abc_Obj_t * pConst1 = Abc_AigConst1( Test.pNtk );

    if ( pConst1 )
        Abc_ObjSetPartId( pConst1, 0 );
    Abc_ObjSetPartId( Test.pA, 0 );
    Abc_ObjSetPartId( Test.pB, 0 );
    Abc_ObjSetPartId( Test.pC, 1 );
    Abc_ObjSetPartId( Test.pN1, 0 );
    Abc_ObjSetPartId( Test.pN2, 1 );
    Abc_ObjSetPartId( Test.pN3, 2 );
    Abc_ObjSetPartId( Test.pPo, 2 );
    Abc_NtkUpdateCutNets( Test.pNtk );
}

void AssignHopTestPartsPoSeparated( const HopTestNtk & Test )
{
    AssignHopTestParts( Test );
    Abc_ObjSetPartId( Test.pPo, 1 );
    Abc_NtkUpdateCutNets( Test.pNtk );
}

Abc_Ntk_t * CreateLatchTestNtk()
{
    Abc_Ntk_t * pNtk = Abc_NtkAlloc( ABC_NTK_LOGIC, ABC_FUNC_SOP, 1 );
    Abc_Obj_t * pPi = Abc_NtkCreatePi( pNtk );
    Abc_Obj_t * pBi = Abc_NtkCreateBi( pNtk );
    Abc_Obj_t * pLatch = Abc_NtkCreateLatch( pNtk );
    Abc_Obj_t * pBo = Abc_NtkCreateBo( pNtk );
    Abc_Obj_t * pPo = Abc_NtkCreatePo( pNtk );

    Abc_ObjAddFanin( pBi, pPi );
    Abc_ObjAddFanin( pLatch, pBi );
    Abc_ObjAddFanin( pBo, pLatch );
    Abc_ObjAddFanin( pPo, pBo );
    return pNtk;
}

bool TestHopCountAndStats()
{
    int NumParts = -1;
    int CutSize = -1;
    int HopNum = -1;
    int MinSize = -1;
    int MaxSize = -1;
    float AvgSize = -1.0f;

    HopTestNtk Test = CreateHopTestNtk();
    Abc_Ntk_t * pNtk = Test.pNtk;
    AssignHopTestParts( Test );

    int ComputedCutSize = Abc_NtkComputeCutSize( pNtk );
    int ComputedHopNum = Abc_NtkComputeHopNum( pNtk );
    Abc_NtkSetPartStats( pNtk, 3, ComputedCutSize, ComputedHopNum );

    bool Ok = true;
    Ok &= ExpectEqual( "cut size", ComputedCutSize, 3 );
    Ok &= ExpectEqual( "hop num", ComputedHopNum, 2 );
    Ok &= Abc_NtkGetPartStats( pNtk, &NumParts, &CutSize, &HopNum, &AvgSize, &MinSize, &MaxSize );
    Ok &= ExpectEqual( "stored num parts", NumParts, 3 );
    Ok &= ExpectEqual( "stored cut size", CutSize, 3 );
    Ok &= ExpectEqual( "stored hop num", HopNum, 2 );
    Ok &= ExpectEqual( "stored min size", MinSize, 2 );
    Ok &= ExpectEqual( "stored max size", MaxSize, 3 );

    Abc_NtkDelete( pNtk );
    return Ok;
}

bool TestPoDoesNotAffectCutOrHop()
{
    HopTestNtk Test = CreateHopTestNtk();
    Abc_Ntk_t * pNtk = Test.pNtk;
    AssignHopTestPartsPoSeparated( Test );

    const int ComputedCutSize = Abc_NtkComputeCutSize( pNtk );
    const int ComputedHopNum = Abc_NtkComputeHopNum( pNtk );

    bool Ok = true;
    Ok &= ExpectEqual( "cut size ignores po", ComputedCutSize, 3 );
    Ok &= ExpectEqual( "hop num ignores po", ComputedHopNum, 2 );
    Ok &= ExpectFalse( "po-only driver is not cut net", Abc_ObjIsCutNet( Test.pN3 ) != 0 );

    Abc_NtkDelete( pNtk );
    return Ok;
}

bool TestInvalidPartIdFails()
{
    HopTestNtk Test = CreateHopTestNtk();
    Abc_Ntk_t * pNtk = Test.pNtk;
    AssignHopTestParts( Test );
    Abc_ObjClearPartId( Test.pN2 );

    const int HopNum = Abc_NtkComputeHopNum( pNtk );
    Abc_NtkDelete( pNtk );
    return ExpectEqual( "invalid part id should fail", HopNum, -1 );
}

bool TestGetPartStatsFailsOnInvalidHop()
{
    int NumParts = -1;
    int CutSize = -1;
    int HopNum = -1;
    int MinSize = -1;
    int MaxSize = -1;
    float AvgSize = -1.0f;

    HopTestNtk Test = CreateHopTestNtk();
    Abc_Ntk_t * pNtk = Test.pNtk;
    AssignHopTestParts( Test );
    Abc_ObjClearPartId( Test.pN2 );

    const bool StatsOk = Abc_NtkGetPartStats( pNtk, &NumParts, &CutSize, &HopNum, &AvgSize, &MinSize, &MaxSize ) != 0;
    Abc_NtkDelete( pNtk );
    return ExpectFalse( "part stats should fail when hop fails", StatsOk );
}

bool TestLatchFails()
{
    Abc_Ntk_t * pNtk = CreateLatchTestNtk();
    const int HopNum = Abc_NtkComputeHopNum( pNtk );
    Abc_NtkDelete( pNtk );
    return ExpectEqual( "latch should fail", HopNum, -1 );
}

} // namespace

int main()
{
    bool Ok = true;
    Ok &= TestHopCountAndStats();
    Ok &= TestPoDoesNotAffectCutOrHop();
    Ok &= TestInvalidPartIdFails();
    Ok &= TestGetPartStatsFailsOnInvalidHop();
    Ok &= TestLatchFails();
    return Ok ? 0 : 1;
}
