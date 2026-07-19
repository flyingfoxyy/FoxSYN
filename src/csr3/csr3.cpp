#include "csr3/csr3.hpp"
#include "csr3/csr3_internal.hpp"

#include <cstdio>
#include <vector>

#include "base/abc/abc.h"
#include "base/abc/abcPdb.hpp"

namespace fox::csr3 {

std::vector<Abc_Obj_t*> collect_crossing_signals(Abc_Ntk_t *pNtk, int srcPart)
{
    std::vector<Abc_Obj_t*> out;
    Abc_Obj_t *pObj, *pFanout;
    int i, j;
    Abc_NtkForEachNode(pNtk, pObj, i)
    {
        if ((int)Abc_ObjGetPartId(pObj) != srcPart)
            continue;
        bool crosses = false;
        Abc_ObjForEachFanout(pObj, pFanout, j)
        {
            if (Abc_ObjIsNode(pFanout) &&
                Abc_PartIdIsValid(Abc_ObjGetPartId(pFanout)) &&
                (int)Abc_ObjGetPartId(pFanout) != srcPart)
            { crosses = true; break; }
        }
        if (crosses)
            out.push_back(pObj);
    }
    return out;
}

int ceil_log2(long m)
{
    if (m <= 1)
        return 0;
    int bits = 0;
    long v = m - 1;
    while (v > 0) { v >>= 1; ++bits; }
    return bits;
}

bool RunCsr3(Abc_Ntk_t *pNtk, const Config &cfg)
{
    if (!pNtk) { printf("csr3: current network is empty\n"); return false; }
    if (!Abc_NtkIsLogic(pNtk)) { printf("csr3: network must be logic (not AIG)\n"); return false; }
    if (!pNtk->pPdb) { printf("csr3: no partition database (run hpart first)\n"); return false; }
    if (Abc_NtkPdb(pNtk)->num_parts() != 2) {
        printf("csr3: v1 only supports N=2 partitions (got %d)\n", Abc_NtkPdb(pNtk)->num_parts());
        return false;
    }
    (void)cfg;
    printf("csr3: scaffold OK (measurement not yet implemented)\n");
    return true;
}

} // namespace fox::csr3
