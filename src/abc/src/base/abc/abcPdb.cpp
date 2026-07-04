/**CFile****************************************************************

  FileName    [abcPdb.cpp]

  SystemName  [ABC: Logic synthesis and verification system.]

  PackageName [Network and node package.]

  Synopsis    [Physical partition database and APIs.]

***********************************************************************/

#include "abcPdb.hpp"
#include "abcInt.h"

ABC_NAMESPACE_IMPL_START

static inline void Abc_NtkInvalidatePartStats( Abc_Ntk_t * pNtk )
{
    if ( pNtk && pNtk->pPdb )
        pNtk->pPdb->invalidate_stats();
}

static inline Abc_Obj_t * Abc_NtkPartObj( Abc_Ntk_t * pNtk, int ObjId )
{
    if ( pNtk == NULL || ObjId < 0 || ObjId >= Abc_NtkObjNumMax(pNtk) )
        return NULL;
    return Abc_NtkObj( pNtk, ObjId );
}

static inline int Abc_ObjHasPartId( Abc_Obj_t * pObj )
{
    return pObj && Abc_PartIdIsValid( Abc_ObjGetPartId( pObj ) );
}

static inline int Abc_ObjIsPartStatVertex( Abc_Obj_t * pObj )
{
    return pObj
        && ( Abc_ObjIsPi( pObj )
          || Abc_ObjIsNode( pObj )
          || Abc_ObjType( pObj ) == ABC_OBJ_CONST1 );
}

struct Abc_CutNetInfo
{
    std::vector<char> IsCutNet;
    int CutSize = 0;
};

static Abc_CutNetInfo Abc_NtkCollectCutNets( Abc_Ntk_t * pNtk )
{
    Abc_CutNetInfo Info;
    Abc_Obj_t * pObj;
    Abc_Obj_t * pFanin;
    int i, k;

    if ( pNtk == NULL )
        return Info;

    Info.IsCutNet.assign( Abc_NtkObjNumMax(pNtk), 0 );
    Abc_NtkForEachObj( pNtk, pObj, i )
    {
        part_id ObjPart;

        if ( !Abc_ObjIsPartStatVertex( pObj ) || !Abc_ObjHasPartId( pObj ) )
            continue;
        ObjPart = Abc_ObjGetPartId( pObj );
        Abc_ObjForEachFanin( pObj, pFanin, k )
        {
            if ( !Abc_ObjIsPartStatVertex( pFanin ) || !Abc_ObjHasPartId( pFanin ) )
                continue;
            if ( Abc_ObjGetPartId( pFanin ) != ObjPart && !Info.IsCutNet[pFanin->Id] )
            {
                Info.IsCutNet[pFanin->Id] = 1;
                Info.CutSize += 1;
            }
        }
    }
    return Info;
}

static inline int Abc_NtkUpdateHopLevel( Abc_Obj_t * pObj, std::vector<int> & HopLevels, int & HopNum )
{
    Abc_Obj_t * pFanin;
    int i;
    int Best = 0;
    part_id ObjPart;

    if ( pObj == NULL || !Abc_ObjHasPartId( pObj ) )
        return 1;

    ObjPart = Abc_ObjGetPartId( pObj );
    Abc_ObjForEachFanin( pObj, pFanin, i )
    {
        int Candidate;

        if ( !Abc_ObjHasPartId( pFanin ) )
            continue;
        Candidate = HopLevels[pFanin->Id] + ( Abc_ObjGetPartId( pFanin ) != ObjPart );
        if ( Candidate > Best )
            Best = Candidate;
    }

    HopLevels[pObj->Id] = Best;
    if ( Best > HopNum )
        HopNum = Best;
    return 1;
}

Pdb * Abc_PdbAlloc()
{
    return new Pdb();
}

void Abc_PdbFree( Pdb * pPdb )
{
    delete pPdb;
}

part_id Abc_NtkGetPartId( Abc_Ntk_t * pNtk, int ObjId )
{
    if ( pNtk == NULL || pNtk->pPdb == NULL )
        return ABC_PART_ID_NONE;
    return pNtk->pPdb->get( ObjId );
}

void Abc_NtkSetPartId( Abc_Ntk_t * pNtk, int ObjId, part_id PartId )
{
    Abc_Obj_t * pObj = Abc_NtkPartObj( pNtk, ObjId );

    if ( pObj == NULL )
        return;
    if ( !Abc_PartIdIsValid( PartId ) )
    {
        if ( pNtk->pPdb )
        {
            pNtk->pPdb->clear( ObjId );
            pNtk->pPdb->invalidate_stats();
        }
    }
    else
    {
        if ( pNtk->pPdb == NULL )
            pNtk->pPdb = new Pdb( Abc_NtkObjNumMax(pNtk) );
        pNtk->pPdb->set( ObjId, PartId );
        pNtk->pPdb->invalidate_stats();
    }
}

void Abc_NtkClearPartId( Abc_Ntk_t * pNtk, int ObjId )
{
    Abc_NtkSetPartId( pNtk, ObjId, ABC_PART_ID_NONE );
}

void Abc_NtkClearPartIds( Abc_Ntk_t * pNtk )
{
    if ( pNtk == NULL )
        return;
    if ( pNtk->pPdb )
    {
        Abc_PdbFree( pNtk->pPdb );
        pNtk->pPdb = NULL;
    }
}

void Abc_NtkUpdateCutNets( Abc_Ntk_t * pNtk )
{
    if ( pNtk == NULL )
        return;
    Abc_NtkInvalidatePartStats( pNtk );
}

int Abc_NtkComputeCutSize( Abc_Ntk_t * pNtk )
{
    if ( pNtk == NULL )
        return -1;

    return Abc_NtkCollectCutNets( pNtk ).CutSize;
}

int Abc_NtkComputeCutEdgeNum( Abc_Ntk_t * pNtk )
{
    Abc_Obj_t * pObj;
    Abc_Obj_t * pFanin;
    int i, k;
    int CutEdge = 0;

    if ( pNtk == NULL )
        return -1;

    // Count every cross-partition (driver, consumer) pair separately. A net
    // with several cross-partition fanouts contributes 1 to cut-net but one
    // cut-edge per crossing fanout.
    Abc_NtkForEachObj( pNtk, pObj, i )
    {
        part_id ObjPart;
        if ( !Abc_ObjIsPartStatVertex( pObj ) || !Abc_ObjHasPartId( pObj ) )
            continue;
        ObjPart = Abc_ObjGetPartId( pObj );
        Abc_ObjForEachFanin( pObj, pFanin, k )
        {
            if ( !Abc_ObjIsPartStatVertex( pFanin ) || !Abc_ObjHasPartId( pFanin ) )
                continue;
            if ( Abc_ObjGetPartId( pFanin ) != ObjPart )
                CutEdge += 1;
        }
    }
    return CutEdge;
}

int Abc_NtkComputeHopNum( Abc_Ntk_t * pNtk )
{
    std::vector<int> HopLevels;
    Vec_Ptr_t * vNodes;
    Abc_Obj_t * pObj;
    int i;
    int HopNum = 0;

    if ( pNtk == NULL )
        return -1;

    HopLevels.resize( Abc_NtkObjNumMax(pNtk), 0 );
    vNodes = Abc_NtkDfs( pNtk, 1 );

    Vec_PtrForEachEntry( Abc_Obj_t *, vNodes, pObj, i )
    {
        if ( !Abc_ObjIsPartStatVertex( pObj ) )
            continue;
        if ( !Abc_NtkUpdateHopLevel( pObj, HopLevels, HopNum ) )
        {
            Vec_PtrFree( vNodes );
            return -1;
        }
    }
    Vec_PtrFree( vNodes );
    return HopNum;
}

void Abc_NtkSetPartStats( Abc_Ntk_t * pNtk, int NumParts, int CutSize, int HopNum )
{
    if ( pNtk == NULL || pNtk->pPdb == NULL )
        return;
    pNtk->pPdb->set_stats( NumParts, CutSize, HopNum );
}

int Abc_NtkGetPartStats( Abc_Ntk_t * pNtk, int * pNumParts, int * pCutSize, int * pHopNum, float * pAvgSize, int * pMinSize, int * pMaxSize )
{
    int Counts[ABC_PART_ID_NONE] = {0};
    int nAssigned = 0;
    int nDistinct = 0;
    int nParts = 0;
    int MinSize = 0;
    int MaxSize = 0;
    int CutSize = 0;
    int HopNum = 0;
    int i;
    Abc_Obj_t * pObj;

    if ( pNtk == NULL || pNtk->pPdb == NULL )
        return 0;

    Abc_NtkForEachObj( pNtk, pObj, i )
    {
        part_id PartId = Abc_ObjGetPartId( pObj );
        if ( !Abc_PartIdIsValid( PartId ) )
            continue;
        Counts[PartId]++;
        nAssigned++;
    }

    for ( i = 0; i < ABC_PART_ID_NONE; ++i )
        nDistinct += Counts[i] > 0;

    nParts = pNtk->pPdb->num_parts() > 0 ? pNtk->pPdb->num_parts() : nDistinct;
    if ( nParts <= 0 )
        return 0;

    MinSize = Counts[0];
    MaxSize = Counts[0];
    for ( i = 1; i < nParts; ++i )
    {
        if ( Counts[i] < MinSize )
            MinSize = Counts[i];
        if ( Counts[i] > MaxSize )
            MaxSize = Counts[i];
    }

    CutSize = pNtk->pPdb->cut_size() >= 0 ? pNtk->pPdb->cut_size() : Abc_NtkComputeCutSize( pNtk );
    HopNum = pNtk->pPdb->hop_num() >= 0 ? pNtk->pPdb->hop_num() : Abc_NtkComputeHopNum( pNtk );
    if ( CutSize < 0 || HopNum < 0 )
        return 0;

    if ( pNumParts )
        *pNumParts = nParts;
    if ( pCutSize )
        *pCutSize = CutSize;
    if ( pHopNum )
        *pHopNum = HopNum;
    if ( pAvgSize )
        *pAvgSize = nParts > 0 ? (float)nAssigned / (float)nParts : 0.0f;
    if ( pMinSize )
        *pMinSize = MinSize;
    if ( pMaxSize )
        *pMaxSize = MaxSize;
    return 1;
}

part_id Abc_ObjGetPartId( Abc_Obj_t * pObj )
{
    if ( pObj == NULL )
        return ABC_PART_ID_NONE;
    return Abc_NtkGetPartId( pObj->pNtk, pObj->Id );
}

void Abc_ObjSetPartId( Abc_Obj_t * pObj, part_id PartId )
{
    if ( pObj == NULL )
        return;
    Abc_NtkSetPartId( pObj->pNtk, pObj->Id, PartId );
}

void Abc_ObjClearPartId( Abc_Obj_t * pObj )
{
    if ( pObj == NULL )
        return;
    Abc_NtkClearPartId( pObj->pNtk, pObj->Id );
}

ABC_NAMESPACE_IMPL_END
