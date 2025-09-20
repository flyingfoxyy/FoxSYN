/*---------------------------------------------------------------------------=\
|                                                                             |
| file:      main.cpp                                                         |
| author:    longfei                                                          |
| purpose:   FoxSYN entry and commands register                               |
| version:   0.1                                                              |
| date:      2025-3-23                                                        |
\---------------------------------------------------------------------------=*/

#include <iostream>

#include "misc/util/abc_global.h"
#include "base/main/main.h"
#include "base/main/mainInt.h"

#include "fox/foxmap.hpp"
#include "supper/foxmap.hpp"

extern "C"
{
    int Abc_RealMain(int argc, char *argv[]);
}

int Foxmap_Command(Abc_Frame_t *pAbc, int argc, char **argv)
{
    using namespace fox::foxmap;
    fox::foxmap::Param param;

    Abc_Ntk_t *pMapped = nullptr;
    Abc_Ntk_t *pAig = nullptr;

    for (int i = 1; i != argc; ++i)
    {
        const char arg = *(argv[i] + 1);
        switch (arg)
        {
        case 'a':
            param.tar = OptTarget::Area;
            break;
        case 'e':
            param.expand_cut ^= 1;
            break;
        case 'r':
            param.tar = OptTarget::Routability;
            break;
        case 'p':
            param.praetor_premap ^= 1;
            break;
        case 'P':
            param.parallel ^= 1;
            break;
        case 'v':
            param.verbose ^= 1;
            break;
        case 'C':
            param.c_value = std::atoi(argv[++i]);
            if (param.c_value < 0)
            {
                printf("foxmap: invalid cut number %ld\n", param.c_value);
                return 1;
            }
            break;
        case 'D':
            param.required = std::atoi(argv[++i]);
            if (param.required < 0)
            {
                printf("foxmap: invalid required time %ld\n", param.required);
                return 1;
            }
            break;
        case 'E':
            param.exact_pass_num = std::atoi(argv[++i]);
            if (param.exact_pass_num < 0)
            {
                printf("foxmap: invalid pass number %ld\n", param.exact_pass_num);
                return 1;
            }
            break;
        case 'F':
            param.flow_pass_num = std::atoi(argv[++i]);
            if (param.flow_pass_num < 0)
            {
                printf("foxmap: invalid pass number %ld\n", param.flow_pass_num);
                return 1;
            }
            break;
        case 'k':
            param.lut_size = std::atoi(argv[++i]);
            if (param.lut_size > 6 || param.lut_size < 2)
            {
                printf("foxmap: invalid LUT size %ld\n", param.lut_size);
                return 1;
            }
            break;
        case 'h':
            goto usage;
        default:
            std::cout << "foxmap: unknown argument -" << arg << "\n";
            goto usage;
            break;
        }
    }

    pAig = Abc_FrameReadNtk(pAbc);

    if (!pAig)
    {
        printf("foxmap: current network is empty\n");
        return 1;
    }

    if (!Abc_NtkIsStrash(pAig))
    {
        printf("foxmap: current network is not an AIG\n");
        return 1;
    }

    pMapped = fox::PerformFoxMap(pAig, &param);

    if (!pMapped)
    {
        printf("foxmap: technology mapping failed\n");
        return 1;
    }

    Abc_FrameReplaceCurrentNetwork( pAbc, pMapped );

    return 0;

usage:
    Abc_Print(-2, "usage: foxmap [-kEFA num] [-av]\n");
    Abc_Print(-2, "\t           performs FPGA technology mapping of the network\n");
    Abc_Print(-2, "\t-k num   : the number of LUT inputs (2 < num < %d) [default = %d]\n", kMaxLutSize + 1, kMaxLutSize);
    Abc_Print(-2, "\t-F num   : the number of area flow iterations (num >= 0) [default = %d]\n", param.flow_pass_num);
    Abc_Print(-2, "\t-E num   : the number of exact area iterations (num >= 0) [default = %d]\n", param.exact_pass_num);
    Abc_Print(-2, "\t-a       : toggles area-oriented technology mapping\n");
    Abc_Print(-2, "\t-e       : toggles cut expandsion, default is %s\n", param.expand_cut);
    Abc_Print(-2, "\t-r       : toggles routability-oriented technology mapping\n");
    Abc_Print(-2, "\t-p       : toggles pre-mapping with praetor algorithm\n");
    Abc_Print(-2, "\t-v       : toggles verbose log print\n");
    Abc_Print(-2, "\n");

    return 1;
}

int Suppermap_Command(Abc_Frame_t *pAbc, int argc, char **argv)
{
    using namespace fox::supper;
    fox::supper::Param param;

    Abc_Ntk_t *pMapped = nullptr;
    Abc_Ntk_t *pAig = nullptr;

    for (int i = 1; i != argc; ++i)
    {
        const char arg = *(argv[i] + 1);
        switch (arg)
        {
        case 'a':
            param.tar = OptTarget::Area;
            break;
        case 'e':
            param.expand_cut ^= 1;
            break;
        case 'r':
            param.tar = OptTarget::Routability;
            break;
        case 'p':
            param.praetor_premap ^= 1;
            break;
        case 'P':
            param.parallel ^= 1;
            break;
        case 'v':
            param.verbose ^= 1;
            break;
        case 'C':
            param.c_value = std::atoi(argv[++i]);
            if (param.c_value < 0)
            {
                printf("foxmap: invalid cut number %ld\n", param.c_value);
                return 1;
            }
            break;
        case 'D':
            param.required = std::atoi(argv[++i]);
            if (param.required < 0)
            {
                printf("foxmap: invalid required time %ld\n", param.required);
                return 1;
            }
            break;
        case 'E':
            param.exact_pass_num = std::atoi(argv[++i]);
            if (param.exact_pass_num < 0)
            {
                printf("foxmap: invalid pass number %ld\n", param.exact_pass_num);
                return 1;
            }
            break;
        case 'F':
            param.flow_pass_num = std::atoi(argv[++i]);
            if (param.flow_pass_num < 0)
            {
                printf("foxmap: invalid pass number %ld\n", param.flow_pass_num);
                return 1;
            }
            break;
        case 'k':
            param.lut_size = std::atoi(argv[++i]);
            if (param.lut_size > 6 || param.lut_size < 2)
            {
                printf("foxmap: invalid LUT size %ld\n", param.lut_size);
                return 1;
            }
            break;
        case 'h':
            goto usage;
        default:
            std::cout << "foxmap: unknown argument -" << arg << "\n";
            goto usage;
            break;
        }
    }

    pAig = Abc_FrameReadNtk(pAbc);

    if (!pAig)
    {
        printf("foxmap: current network is empty\n");
        return 1;
    }

    if (!Abc_NtkIsStrash(pAig))
    {
        printf("foxmap: current network is not an AIG\n");
        return 1;
    }

    pMapped = fox::perform_supper_map(pAig, &param);

    if (!pMapped)
    {
        printf("foxmap: technology mapping failed\n");
        return 1;
    }

    Abc_FrameReplaceCurrentNetwork( pAbc, pMapped );

    return 0;

usage:
    Abc_Print(-2, "usage: smap [-kEFA num] [-av]\n");
    Abc_Print(-2, "\t           performs FPGA technology mapping of the network\n");
    Abc_Print(-2, "\t-k num   : the number of LUT inputs (2 < num < %d) [default = %d]\n", kMaxLutSize + 1, kMaxLutSize);
    Abc_Print(-2, "\t-F num   : the number of area flow iterations (num >= 0) [default = %d]\n", param.flow_pass_num);
    Abc_Print(-2, "\t-E num   : the number of exact area iterations (num >= 0) [default = %d]\n", param.exact_pass_num);
    Abc_Print(-2, "\t-a       : toggles area-oriented technology mapping\n");
    Abc_Print(-2, "\t-e       : toggles cut expandsion, default is %s\n", param.expand_cut);
    Abc_Print(-2, "\t-r       : toggles routability-oriented technology mapping\n");
    Abc_Print(-2, "\t-p       : toggles pre-mapping with praetor algorithm\n");
    Abc_Print(-2, "\t-v       : toggles verbose log print\n");
    Abc_Print(-2, "\n");

    return 1;
}

struct CmdRegister
{
    CmdRegister()
    {
        Cmd_CommandAdd(Abc_FrameGetGlobalFrame(), "FoxSYN", "foxmap", Foxmap_Command, 1);
        Cmd_CommandAdd(Abc_FrameGetGlobalFrame(), "FoxSYN", "smap",   Suppermap_Command, 1);
    }
} regiter;

int main(int argc, char *argv[])
{
    return ABC_NAMESPACE_PREFIX Abc_RealMain(argc, argv);
}