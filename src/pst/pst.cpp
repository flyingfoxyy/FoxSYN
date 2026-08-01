#include "pst.hpp"

#include "base/abc/abc.h"
#include "base/abc/abcPdb.hpp"

#include <cstdio>

namespace fox::pst {

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
