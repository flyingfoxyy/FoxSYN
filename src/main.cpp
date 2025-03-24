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
        case 'v':
            param.verbose = true;
            break;
        case 'k':
            param.lut_size = *(argv[++i] + 1) - '0';
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
    Abc_Print(-2, "\t-E num   : the number of effective area iterations (num >= 0) [default = %d]\n", param.praetor_pass_num);
    Abc_Print(-2, "\t-F num   : the number of area flow iterations (num >= 0) [default = %d]\n", param.flow_pass_num);
    Abc_Print(-2, "\t-A num   : the number of exact area recovery iterations (num >= 0) [default = %d]\n", param.exact_pass_num);
    Abc_Print(-2, "\t-a       : toggles area-oriented technology mapping\n");
    Abc_Print(-2, "\t-v       : toggles verbose log print\n");
    Abc_Print(-2, "\n");
    Abc_Print(-2, "\t         This command is contribute by Longfei Fan [changqingfans@gmail.com]");

    return 1;
}

struct CmdRegister
{
    CmdRegister()
    {
        Cmd_CommandAdd(Abc_FrameGetGlobalFrame(), "FoxSYN", "foxmap", Foxmap_Command, 1);
    }
} regiter;

int main(int argc, char *argv[])
{
    return ABC_NAMESPACE_PREFIX Abc_RealMain(argc, argv);
}