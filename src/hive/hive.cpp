#include "hive/hive.hpp"

#include "base/abc/abc.h"

namespace fox::hive {

Result RunHive(Abc_Ntk_t *pNtk, const Config &cfg)
{
    (void)cfg;
    Result res;
    if (!pNtk || !Abc_NtkIsLogic(pNtk))
        return res;
    res.ok = true;
    return res;
}

bool ApplyHive(Abc_Frame_t *pAbc, const Config &cfg)
{
    Abc_Ntk_t *pNtk = Abc_FrameReadNtk(pAbc);
    if (!pNtk)
    {
        printf("hive: network is null\n");
        return false;
    }
    if (!Abc_NtkIsLogic(pNtk))
    {
        printf("hive: network must be logic (run if -K first)\n");
        return false;
    }
    Result res = RunHive(pNtk, cfg);
    return res.ok;
}

} // namespace fox::hive
