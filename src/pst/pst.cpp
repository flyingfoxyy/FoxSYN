#include "pst.hpp"

#include "base/abc/abc.h"
#include "base/abc/abcPdb.hpp"

#include <cstdint>
#include <cstdio>
#include <unordered_map>

namespace fox::pst {

// ---------------------------------------------------------------------------
// COUPLING WARNING: layout mirror of ABC's private Abc_Aig_t_.
//
// struct Abc_Aig_t_ is defined only in src/abc/src/base/abc/abcAig.c:52 --
// abc.h:118 has just the forward declaration -- and Abc_AigAndCreate is
// static. Creating a second AND with identical fanins (needed when the two
// belong to different partitions) requires writing into pBins directly, which
// requires knowing the layout.
//
// Fields below MUST match abcAig.c:52-70 verbatim, in order. If ABC is updated
// from upstream and that struct gains/loses/reorders a field, this mirror
// silently reads wrong offsets. Only the first five fields are ever accessed;
// the rest exist solely to make the layout correct.
// ---------------------------------------------------------------------------
struct PstAigMirror
{
    Abc_Ntk_t *       pNtkAig;
    Abc_Obj_t *       pConst1;
    Abc_Obj_t **      pBins;
    int               nBins;
    int               nEntries;
    Vec_Ptr_t *       vNodes;
    Vec_Ptr_t *       vStackReplaceOld;
    Vec_Ptr_t *       vStackReplaceNew;
    Vec_Vec_t *       vLevels;
    Vec_Vec_t *       vLevelsR;
    Vec_Ptr_t *       vAddedCells;
    Vec_Ptr_t *       vUpdatedNets;
    int               nStrash0;
    int               nStrash1;
    int               nStrash5;
    int               nStrash2;
};

// Verbatim copy of Abc_HashKey2 (abcAig.c:90). Must stay bit-identical to
// ABC's, otherwise pst_force_create would file the node in a bin that
// Abc_AigAndLookup/Abc_AigAndDelete would not search.
static unsigned pst_hash_key2(Abc_Obj_t *p0, Abc_Obj_t *p1, int TableSize)
{
    unsigned Key = 0;
    Key ^= Abc_ObjRegular(p0)->Id * 7937;
    Key ^= Abc_ObjRegular(p1)->Id * 2971;
    Key ^= Abc_ObjIsComplement(p0) * 911;
    Key ^= Abc_ObjIsComplement(p1) * 353;
    return Key % TableSize;
}

// Adapted from the static Abc_AigAndCreate (abcAig.c:319). Unconditionally
// creates a new AND and files it in the bin, even when an entry with the same
// (p0, p1) key already exists -- that is the whole point: two ANDs with equal
// fanins but different part_id must coexist.
//
// Two deliberate omissions vs the original:
//   - no Abc_AigResize call: the mirror cannot call the static Abc_AigResize,
//     and forced duplicates are a small minority. Skipping resize only makes a
//     bin chain slightly longer; correctness is unaffected.
//   - no vAddedCells push: that list is for incremental rewriting, which pst
//     does not use.
static Abc_Obj_t *pst_force_create(Abc_Aig_t *pManOpaque, Abc_Obj_t *p0, Abc_Obj_t *p1)
{
    PstAigMirror *pMan = (PstAigMirror *)pManOpaque;
    Abc_Obj_t *pAnd;
    unsigned Key;

    // order the arguments (same convention as Abc_AigAndCreate)
    if (Abc_ObjRegular(p0)->Id > Abc_ObjRegular(p1)->Id)
        pAnd = p0, p0 = p1, p1 = pAnd;

    pAnd = Abc_NtkCreateNode(pMan->pNtkAig);
    Abc_ObjAddFanin(pAnd, p0);
    Abc_ObjAddFanin(pAnd, p1);
    pAnd->Level  = 1 + Abc_MaxInt(Abc_ObjRegular(p0)->Level, Abc_ObjRegular(p1)->Level);
    pAnd->fExor  = Abc_NodeIsExorType(pAnd);
    pAnd->fPhase = (Abc_ObjIsComplement(p0) ^ Abc_ObjRegular(p0)->fPhase)
                 & (Abc_ObjIsComplement(p1) ^ Abc_ObjRegular(p1)->fPhase);

    Key = pst_hash_key2(p0, p1, pMan->nBins);
    pAnd->pNext      = pMan->pBins[Key];
    pMan->pBins[Key] = pAnd;
    pMan->nEntries++;
    pAnd->pCopy = NULL;
    return pAnd;
}

struct PstMan
{
    Abc_Ntk_t *pNtkNew = NULL;
    Abc_Aig_t *pMan    = NULL;
    std::unordered_map<uint64_t, Abc_Obj_t *> table;
    int dup_blocked = 0;
};

// Key layout: [ id0:27 | c0:1 | id1:27 | c1:1 | part:8 ]. Object ids fit in 27
// bits (134M objects) with room to spare for any netlist pst will see; part_id
// is a uint8_t (abc.h:124). Callers must pass p0/p1 already ordered by id.
static inline uint64_t pst_make_key(Abc_Obj_t *p0, Abc_Obj_t *p1, part_id part)
{
    uint64_t id0 = (uint64_t)Abc_ObjRegular(p0)->Id;
    uint64_t id1 = (uint64_t)Abc_ObjRegular(p1)->Id;
    uint64_t c0  = Abc_ObjIsComplement(p0) ? 1u : 0u;
    uint64_t c1  = Abc_ObjIsComplement(p1) ? 1u : 0u;
    return (id0 << 37) | (c0 << 36) | (id1 << 9) | (c1 << 8) | (uint64_t)part;
}

// Partition-aware replacement for Abc_AigAnd. Reuse happens only among ANDs
// with the same part_id; a same-fanin AND belonging to another partition
// forces a duplicate node instead of being shared.
static Abc_Obj_t *pst_and(PstMan &man, Abc_Obj_t *p0, Abc_Obj_t *p1, part_id part)
{
    // Trivial cases resolve to an existing object or a constant, not to a new
    // AND -- they get no part_id and no table entry. Mirrors the head of
    // Abc_AigAndLookup (abcAig.c:410-425).
    Abc_Obj_t *pConst1 = Abc_AigConst1(man.pNtkNew);
    if (p0 == p1)
        return p0;
    if (p0 == Abc_ObjNot(p1))
        return Abc_ObjNot(pConst1);
    if (Abc_ObjRegular(p0) == pConst1)
        return (p0 == pConst1) ? p1 : Abc_ObjNot(pConst1);
    if (Abc_ObjRegular(p1) == pConst1)
        return (p1 == pConst1) ? p0 : Abc_ObjNot(pConst1);

    // Order the arguments so the key is canonical, matching what
    // Abc_AigAndCreate/pst_force_create will store.
    if (Abc_ObjRegular(p0)->Id > Abc_ObjRegular(p1)->Id)
    {
        Abc_Obj_t *pTemp = p0;
        p0 = p1;
        p1 = pTemp;
    }

    uint64_t key = pst_make_key(p0, p1, part);
    auto it = man.table.find(key);
    if (it != man.table.end())
        return it->second; // same-partition reuse

    Abc_Obj_t *pAnd;
    if (Abc_AigAndLookup(man.pMan, p0, p1) == NULL)
    {
        // Not in ABC's table (or dangling, which lookup also reports as NULL):
        // the public API handles creation, resizing, and its own re-lookup.
        pAnd = Abc_AigAnd(man.pMan, p0, p1);
    }
    else
    {
        // ABC already has this (p0, p1) but under a different part_id, so the
        // node cannot be shared. Force a duplicate.
        pAnd = pst_force_create(man.pMan, p0, p1);
        man.dup_blocked++;
    }

    Abc_ObjSetPartId(pAnd, part);
    man.table[key] = pAnd;
    return pAnd;
}

bool ApplyPst(Abc_Frame_t *pAbc)
{
    Abc_Ntk_t *pNtk = Abc_FrameReadNtk(pAbc);
    if (!pNtk)
    {
        printf("pst: network is null\n");
        return false;
    }
    if (!Abc_NtkIsLogic(pNtk))
    {
        printf("pst: network must be logic (not AIG)\n");
        return false;
    }
    if (!pNtk->pPdb)
    {
        printf("pst: no partition database (run hpart first)\n");
        return false;
    }

    printf("pst: skeleton reached, not yet implemented\n");
    return false;
}

} // namespace fox::pst
