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
#include "supper/map.hpp"
#include "partsyn/partsyn.hpp"

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

namespace fox::supper {
    Abc_Ntk_t *PerformSupperMap(Abc_Ntk_t *pNtk, const Config &cfg);
}

int Suppermap_Command(Abc_Frame_t *pAbc, int argc, char **argv)
{
    using namespace fox::supper;
    Config cfg;
    std::string dot_file; // Path to output DOT file
    Abc_Ntk_t *pMapped = nullptr;
    Abc_Ntk_t *pAig = nullptr;
    mapper *mgr = nullptr;

    // Print usage if requested
    if (argc > 1 && !strcmp(argv[1], "-h"))
    {
        goto usage;
    }
        
    // Parse command line arguments
    for (int i = 1; i != argc; ++i)
    {
        const char arg = *(argv[i] + 1);
        switch (arg)
        {
        case 'a':
            cfg.opt_target = Config::target_t::AREA;
            break;
        case 'g':
            cfg.map_impl = Config::map_impl_t::AGDMAP;
            break;
        case 'd':
            // Export graph to DOT format
            if (i + 1 < argc)
            {
                dot_file = argv[++i];
            }
            else
            {
                printf("Error: -d requires a file path\n");
                return 1;
            }
            break;
        default:
            std::cout << "foxmap: unknown argument -" << arg << "\n";
            goto usage;
        }
    }

    // Get the current network
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

    // Create mapper
    mgr = mapper::create_from_aig(static_cast<void *>(pAig));
    if (!mgr)
    {
        printf("foxmap: failed to create mapper\n");
        return 1;
    }
    
    // If dot_file is provided, export the graph to DOT format
    if (!dot_file.empty())
    {
        printf("Exporting graph to DOT file: %s\n", dot_file.c_str());
        if (mgr->to_dot(dot_file))
        {
            printf("Successfully wrote DOT file\n");
        }
        else
        {
            printf("Failed to write DOT file\n");
            delete mgr;
            return 1;
        }
    }
    
    // Perform mapping
    pMapped = PerformSupperMap(pAig, cfg);
    if (!pMapped)
    {
        printf("foxmap: technology mapping failed\n");
        delete mgr;
        return 1;
    }
    
    // Clean up and return
    if (mgr) delete mgr;
    Abc_FrameReplaceCurrentNetwork(pAbc, pMapped);
    return 0;

usage:
    Abc_Print(-2, "usage: super [-kEFA num] [-av] [-d dot_file]\n");
    Abc_Print(-2, "\t           performs FPGA technology mapping of the network\n");
    // Abc_Print(-2, "\t-k num   : the number of LUT inputs (2 < num < %d) [default = %d]\n", kMaxLutSize + 1, kMaxLutSize);
    Abc_Print(-2, "\t-a       : toggles area-oriented technology mapping\n");
    Abc_Print(-2, "\t-v       : toggles verbose log print\n");
    Abc_Print(-2, "\t-d file  : exports the graph structure to DOT file\n");
    Abc_Print(-2, "\n");
    return 1;
}

int PartSyn_Command(Abc_Frame_t *pAbc, int argc, char **argv)
{
    using namespace fox::partsyn;
    Config cfg;
    Abc_Ntk_t *pResult = nullptr;
    Abc_Ntk_t *pAig = nullptr;

    // Print usage if requested
    if (argc > 1 && !strcmp(argv[1], "-h"))
    {
        goto usage;
    }

    // Parse command line arguments
    for (int i = 1; i != argc; ++i)
    {
        const char arg = *(argv[i] + 1);
        switch (arg)
        {
        case 'n':
            cfg.num_parts = std::atoi(argv[++i]);
            if (cfg.num_parts < 2)
            {
                printf("partsyn: invalid partition number %d (must be >= 2)\n", cfg.num_parts);
                return 1;
            }
            break;
        case 'v':
            cfg.verbose ^= 1;
            break;
        case 'h':
            goto usage;
        default:
            std::cout << "partsyn: unknown argument -" << arg << "\n";
            goto usage;
        }
    }

    // Get the current network
    pAig = Abc_FrameReadNtk(pAbc);
    if (!pAig)
    {
        printf("partsyn: current network is empty\n");
        return 1;
    }

    if (!Abc_NtkIsStrash(pAig))
    {
        printf("partsyn: current network is not an AIG\n");
        return 1;
    }

    // Perform partition-based synthesis
    pResult = PerformPartSyn(pAig, cfg);
    if (!pResult)
    {
        printf("partsyn: partition-based synthesis failed\n");
        return 1;
    }

    // Replace current network
    Abc_FrameReplaceCurrentNetwork(pAbc, pResult);
    return 0;

usage:
    Abc_Print(-2, "usage: partsyn [-n num] [-v]\n");
    Abc_Print(-2, "\t           performs partition-based parallel synthesis\n");
    Abc_Print(-2, "\t-n num   : number of partitions (num >= 2) [default = %d]\n", cfg.num_parts);
    Abc_Print(-2, "\t-v       : toggles verbose output\n");
    Abc_Print(-2, "\n");
    return 1;
}

struct CmdRegister
{
    CmdRegister()
    {
        Cmd_CommandAdd(Abc_FrameGetGlobalFrame(), "FoxSYN", "foxmap", Foxmap_Command, 1);
        Cmd_CommandAdd(Abc_FrameGetGlobalFrame(), "FoxSYN", "smap",   Suppermap_Command, 1);
        Cmd_CommandAdd(Abc_FrameGetGlobalFrame(), "FoxSYN", "partsyn", PartSyn_Command, 1);
    }
} regiter;

int main(int argc, char *argv[])
{
    return ABC_NAMESPACE_PREFIX Abc_RealMain(argc, argv);
}
