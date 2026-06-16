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
#include "hpart/hpart.hpp"
#include "timer/timer.hpp"
#include "cpr/cpr.hpp"
#include "cmfs/cmfs.hpp"
#include "agdmap/AgdmapCommand.h"
#include "curvemap/curvemap.h"

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

int Timer_Command(Abc_Frame_t *pAbc, int argc, char **argv)
{
    fox::timer::Config cfg;
    Abc_Ntk_t *pNtk = Abc_FrameReadNtk(pAbc);

    if (argc > 1 && !strcmp(argv[1], "-h"))
    {
        goto usage;
    }

    for (int i = 1; i != argc; ++i)
    {
        if (argv[i][0] != '-')
        {
            std::cout << "timer: unexpected argument " << argv[i] << "\n";
            goto usage;
        }
        const char arg = *(argv[i] + 1);
        switch (arg)
        {
        case 'N':
            if (i + 1 >= argc)
            {
                printf("timer: -N requires a number\n");
                return 1;
            }
            cfg.top_n = std::atoi(argv[++i]);
            if (cfg.top_n < 1)
            {
                printf("timer: invalid path count %d (must be >= 1)\n", cfg.top_n);
                return 1;
            }
            break;
        case 'v':
            cfg.verbose ^= 1;
            break;
        case 'h':
            goto usage;
        default:
            std::cout << "timer: unknown argument -" << arg << "\n";
            goto usage;
        }
    }

    if (!pNtk)
    {
        printf("timer: current network is empty\n");
        return 1;
    }

    return fox::timer::RunTimer(pNtk, cfg) ? 0 : 1;

usage:
    Abc_Print(-2, "usage: timer [-N num] [-v]\n");
    Abc_Print(-2, "\t           simple hop-aware STA report on the current network\n");
    Abc_Print(-2, "\t-N num  : print top num critical paths [default = %d]\n", cfg.top_n);
    Abc_Print(-2, "\t-v      : toggles verbose output\n");
    Abc_Print(-2, "\n");
    return 1;
}

int Cpr_Command(Abc_Frame_t *pAbc, int argc, char **argv)
{
    fox::cpr::Config cfg;
    Abc_Ntk_t *pNtk = Abc_FrameReadNtk(pAbc);

    if (argc > 1 && !strcmp(argv[1], "-h"))
    {
        goto usage;
    }

    for (int i = 1; i != argc; ++i)
    {
        if (argv[i][0] != '-')
        {
            std::cout << "cpr: unexpected argument " << argv[i] << "\n";
            goto usage;
        }
        const char arg = *(argv[i] + 1);
        switch (arg)
        {
        case 'B':
            if (i + 1 >= argc)
            {
                printf("cpr: -B requires a balance percentage\n");
                return 1;
            }
            cfg.balance_pct = std::atoi(argv[++i]);
            if (cfg.balance_pct < 1 || cfg.balance_pct > 99)
            {
                printf("cpr: invalid balance percentage %d (must be between 1 and 99)\n", cfg.balance_pct);
                return 1;
            }
            break;
        case 'G':
            if (i + 1 >= argc)
            {
                printf("cpr: -G requires a growth percentage\n");
                return 1;
            }
            cfg.replicate_growth_pct = std::atoi(argv[++i]);
            if (cfg.replicate_growth_pct < 0 || cfg.replicate_growth_pct > 100)
            {
                printf("cpr: invalid growth percentage %d (must be between 0 and 100)\n", cfg.replicate_growth_pct);
                return 1;
            }
            break;
        case 'C':
            if (i + 1 >= argc)
            {
                printf("cpr: -C requires a cutsize growth percentage\n");
                return 1;
            }
            cfg.cutsize_growth_pct = std::atoi(argv[++i]);
            if (cfg.cutsize_growth_pct < 0 || cfg.cutsize_growth_pct > 999)
            {
                printf("cpr: invalid cutsize growth percentage %d (must be between 0 and 999)\n", cfg.cutsize_growth_pct);
                return 1;
            }
            break;
        case 'r':
            if (i + 1 >= argc)
            {
                printf("cpr: -r requires a round count\n");
                return 1;
            }
            cfg.relocate_max_rounds = std::atoi(argv[++i]);
            if (cfg.relocate_max_rounds < 0)
            {
                printf("cpr: invalid relocate round count %d\n", cfg.relocate_max_rounds);
                return 1;
            }
            break;
        case 's':
            if (i + 1 >= argc)
            {
                printf("cpr: -s requires a stall limit\n");
                return 1;
            }
            cfg.relocate_stall_limit = std::atoi(argv[++i]);
            if (cfg.relocate_stall_limit < 1)
            {
                printf("cpr: invalid relocate stall limit %d (must be >= 1)\n", cfg.relocate_stall_limit);
                return 1;
            }
            break;
        case 'R':
            if (i + 1 >= argc)
            {
                printf("cpr: -R requires a round count\n");
                return 1;
            }
            cfg.replicate_max_rounds = std::atoi(argv[++i]);
            if (cfg.replicate_max_rounds < 0)
            {
                printf("cpr: invalid replicate round count %d\n", cfg.replicate_max_rounds);
                return 1;
            }
            break;
        case 'S':
            if (i + 1 >= argc)
            {
                printf("cpr: -S requires a stall limit\n");
                return 1;
            }
            cfg.replicate_stall_limit = std::atoi(argv[++i]);
            if (cfg.replicate_stall_limit < 1)
            {
                printf("cpr: invalid replicate stall limit %d (must be >= 1)\n", cfg.replicate_stall_limit);
                return 1;
            }
            break;
        case 'v':
            cfg.verbose ^= 1;
            break;
        case 'h':
            goto usage;
        default:
            std::cout << "cpr: unknown argument -" << arg << "\n";
            goto usage;
        }
    }

    if (!pNtk)
    {
        printf("cpr: current network is empty\n");
        return 1;
    }

    if (!fox::cpr::ApplyCpr(pNtk, cfg))
        return 1;

    return 0;

usage:
    Abc_Print(-2, "usage: cpr [-B pct] [-G pct] [-C pct] [-r num] [-s num] [-R num] [-S num] [-v]\n");
    Abc_Print(-2, "\t           critical-path delay reduction using partition info\n");
    Abc_Print(-2, "\t           relocate phase runs first, then replicate\n");
    Abc_Print(-2, "\t-B pct  : balance imbalance percent, enforced at end (-1 = inherit pdb) [default = %d]\n", cfg.balance_pct);
    Abc_Print(-2, "\t-G pct  : max node growth from replication, in percent of initial nodes [default = %d]\n", cfg.replicate_growth_pct);
    Abc_Print(-2, "\t-C pct  : max cutsize growth across the whole run, in percent [default = %d]\n", cfg.cutsize_growth_pct);
    Abc_Print(-2, "\t-r num  : max relocate rounds (>= 0) [default = %d]\n", cfg.relocate_max_rounds);
    Abc_Print(-2, "\t-s num  : quit relocate after N consecutive rounds without timing gain (>= 1) [default = %d]\n", cfg.relocate_stall_limit);
    Abc_Print(-2, "\t-R num  : max replicate rounds (>= 0) [default = %d]\n", cfg.replicate_max_rounds);
    Abc_Print(-2, "\t-S num  : quit replicate after N consecutive rounds without timing gain (>= 1) [default = %d]\n", cfg.replicate_stall_limit);
    Abc_Print(-2, "\t-v      : toggles verbose output\n");
    Abc_Print(-2, "\n");
    return 1;
}

int HPart_Command(Abc_Frame_t *pAbc, int argc, char **argv)
{
    using namespace fox::hpart;
    Config cfg;
    Abc_Ntk_t *pNtk = nullptr;

    if (argc > 1 && !strcmp(argv[1], "-h"))
    {
        goto usage;
    }

    for (int i = 1; i != argc; ++i)
    {
        if (argv[i][0] != '-')
        {
            std::cout << "hpart: unexpected argument " << argv[i] << "\n";
            goto usage;
        }

        const char arg = *(argv[i] + 1);
        switch (arg)
        {
        case 'T':
            if (i + 1 >= argc)
            {
                printf("hpart: -T requires a partitioner name\n");
                return 1;
            }
            ++i;
            if (!strcmp(argv[i], "hmetis"))
            {
                cfg.tool = Tool::HMetis;
            }
            else if (!strcmp(argv[i], "shmetis"))
            {
                cfg.tool = Tool::SHMetis;
            }
            else if (!strcmp(argv[i], "kmetis"))
            {
                cfg.tool = Tool::KMetis;
            }
            else
            {
                printf("hpart: invalid partitioner %s (expected hmetis/shmetis/kmetis)\n", argv[i]);
                return 1;
            }
            break;
        case 'N':
            if (i + 1 >= argc)
            {
                printf("hpart: -N requires a partition count\n");
                return 1;
            }
            cfg.num_parts = std::atoi(argv[++i]);
            if (cfg.num_parts < 2 || cfg.num_parts > ABC_PART_ID_NONE)
            {
                printf("hpart: invalid partition number %d (must be between 2 and 255)\n", cfg.num_parts);
                return 1;
            }
            break;
        case 'B':
            if (i + 1 >= argc)
            {
                printf("hpart: -B requires a balance percentage\n");
                return 1;
            }
            cfg.balance_pct = std::atoi(argv[++i]);
            if (cfg.balance_pct < 1 || cfg.balance_pct > 49)
            {
                printf("hpart: invalid balance percentage %d (must be between 1 and 49)\n", cfg.balance_pct);
                return 1;
            }
            break;
        case 'v':
            cfg.verbose ^= 1;
            break;
        case 'h':
            goto usage;
        default:
            // Check for long options (--save-part, --load-part)
            if (strncmp(argv[i], "--save-part", 11) == 0)
            {
                if (i + 1 >= argc) { printf("hpart: --save-part requires a file path\n"); return 1; }
                cfg.save_part = argv[++i];
                break;
            }
            if (strncmp(argv[i], "--load-part", 11) == 0)
            {
                if (i + 1 >= argc) { printf("hpart: --load-part requires a file path\n"); return 1; }
                cfg.load_part = argv[++i];
                break;
            }
            std::cout << "hpart: unknown argument " << argv[i] << "\n";
            goto usage;
        }
    }

    pNtk = Abc_FrameReadNtk(pAbc);
    if (!pNtk)
    {
        printf("hpart: current network is empty\n");
        return 1;
    }

    return ApplyPartitioning(pNtk, cfg) ? 0 : 1;

usage:
    Abc_Print(-2, "usage: hpart [-T hmetis|shmetis|kmetis] [-N num] [-B pct] [--save-part file] [--load-part file] [-v]\n");
    Abc_Print(-2, "\t           partitions the current network structurally and creates a pdb\n");
    Abc_Print(-2, "\t-T name         : partitioner name [default = %s]\n", ToolName(cfg.tool));
    Abc_Print(-2, "\t-N num          : number of partitions (2 <= num <= 255) [default = %d]\n", cfg.num_parts);
    Abc_Print(-2, "\t-B pct          : balance imbalance percent (1 <= pct <= 49) [default = %d]\n", cfg.balance_pct);
    Abc_Print(-2, "\t--save-part file: save partition result to file (for reproducible runs)\n");
    Abc_Print(-2, "\t--load-part file: load partition from file instead of running hmetis\n");
    Abc_Print(-2, "\t-v              : toggles partitioner log output\n");
    Abc_Print(-2, "\n");
    return 1;
}

int Cmfs_Command(Abc_Frame_t *pAbc, int argc, char **argv)
{
    fox::cmfs::Config cfg;
    Abc_Ntk_t *pNtk = Abc_FrameReadNtk(pAbc);

    if (argc > 1 && !strcmp(argv[1], "-h"))
        goto usage;

    for (int i = 1; i != argc; ++i)
    {
        if (argv[i][0] != '-')
        {
            std::cout << "cmfs: unexpected argument " << argv[i] << "\n";
            goto usage;
        }
        const char arg = *(argv[i] + 1);
        switch (arg)
        {
        case 'K':
            if (i + 1 >= argc) { printf("cmfs: -K requires a number\n"); return 1; }
            cfg.top_K = std::atoi(argv[++i]);
            if (cfg.top_K < 1) { printf("cmfs: invalid top_K %d\n", cfg.top_K); return 1; }
            break;
        case 'R':
            if (i + 1 >= argc) { printf("cmfs: -R requires a number\n"); return 1; }
            cfg.max_rounds = std::atoi(argv[++i]);
            if (cfg.max_rounds < 1) { printf("cmfs: invalid max_rounds %d\n", cfg.max_rounds); return 1; }
            break;
        case 'S':
            if (i + 1 >= argc) { printf("cmfs: -S requires a number\n"); return 1; }
            cfg.stall_limit = std::atoi(argv[++i]);
            if (cfg.stall_limit < 1) { printf("cmfs: invalid stall_limit %d\n", cfg.stall_limit); return 1; }
            break;
        case 'C':
            if (i + 1 >= argc) { printf("cmfs: -C requires a number\n"); return 1; }
            cfg.nBTLimit = std::atoi(argv[++i]);
            if (cfg.nBTLimit < 0) { printf("cmfs: invalid BT limit %d\n", cfg.nBTLimit); return 1; }
            break;
        case 'W':
            if (i + 1 >= argc) { printf("cmfs: -W requires a number\n"); return 1; }
            cfg.nWinTfoLevs = std::atoi(argv[++i]);
            if (cfg.nWinTfoLevs < 0) { printf("cmfs: invalid TFO levels %d\n", cfg.nWinTfoLevs); return 1; }
            break;
        case 'F':
            if (i + 1 >= argc) { printf("cmfs: -F requires a number\n"); return 1; }
            cfg.nFanoutsMax = std::atoi(argv[++i]);
            if (cfg.nFanoutsMax < 1) { printf("cmfs: invalid fanout max %d\n", cfg.nFanoutsMax); return 1; }
            break;
        case 'M':
            if (i + 1 >= argc) { printf("cmfs: -M requires a number\n"); return 1; }
            cfg.nWinMax = std::atoi(argv[++i]);
            if (cfg.nWinMax < 0) { printf("cmfs: invalid window max %d\n", cfg.nWinMax); return 1; }
            break;
        case 'X':
            if (i + 1 >= argc) { printf("cmfs: -X requires a number\n"); return 1; }
            cfg.maxTempLut = std::atoi(argv[++i]);
            if (cfg.maxTempLut != 0 && (cfg.maxTempLut < 7 || cfg.maxTempLut > 12))
            { printf("cmfs: -X must be 0 (off) or 7-12\n"); return 1; }
            break;
        case 'D':
            if (i + 1 >= argc) { printf("cmfs: -D requires a number\n"); return 1; }
            cfg.maxWinDepth = std::atoi(argv[++i]);
            if (cfg.maxWinDepth < 0) { printf("cmfs: invalid max window depth %d\n", cfg.maxWinDepth); return 1; }
            break;
        case 'r':
            cfg.allow_resub ^= 1;
            break;
        case 'v':
            cfg.verbose ^= 1;
            break;
        case 'h':
            goto usage;
        default:
            std::cout << "cmfs: unknown argument -" << arg << "\n";
            goto usage;
        }
    }

    if (!pNtk) { printf("cmfs: current network is empty\n"); return 1; }
    if (!Abc_NtkIsLogic(pNtk)) { printf("cmfs: network must be logic (not AIG)\n"); return 1; }
    if (!pNtk->pPdb) { printf("cmfs: no partition database (run hpart first)\n"); return 1; }

    return fox::cmfs::ApplyCmfs(pNtk, cfg) ? 0 : 1;

usage:
    Abc_Print(-2, "usage: cmfs [-K num] [-R num] [-S num] [-C num] [-W num] [-F num] [-M num] [-X num] [-D num] [-rv]\n");
    Abc_Print(-2, "\t           critical-path edge removal using SAT-based redundancy\n");
    Abc_Print(-2, "\t-K num  : number of critical paths to analyze [default = %d]\n", cfg.top_K);
    Abc_Print(-2, "\t-R num  : max optimization rounds [default = %d]\n", cfg.max_rounds);
    Abc_Print(-2, "\t-S num  : stall limit (rounds without improvement) [default = %d]\n", cfg.stall_limit);
    Abc_Print(-2, "\t-C num  : SAT conflict limit per attempt [default = %d]\n", cfg.nBTLimit);
    Abc_Print(-2, "\t-W num  : MFS window TFO levels [default = %d]\n", cfg.nWinTfoLevs);
    Abc_Print(-2, "\t-F num  : MFS max fanouts for window [default = %d]\n", cfg.nFanoutsMax);
    Abc_Print(-2, "\t-M num  : MFS max window node count [default = %d]\n", cfg.nWinMax);
    Abc_Print(-2, "\t-X num  : max temp LUT size for Shannon decomp (0=off, 7-12) [default = %d]\n", cfg.maxTempLut);
    Abc_Print(-2, "\t-D num  : iterative deepening max TFO depth (0=off) [default = %d]\n", cfg.maxWinDepth);
    Abc_Print(-2, "\t-r      : enable partition-aware resubstitution [default = %s]\n", cfg.allow_resub ? "on" : "off");
    Abc_Print(-2, "\t-v      : toggles verbose output\n");
    Abc_Print(-2, "\n");
    return 1;
}

int Curvemap_Command(Abc_Frame_t* pAbc, int argc, char** argv) {
    using namespace fox::curvemap;

    int K = 6;
    int nPasses = 3;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-K") == 0) {
            K = atoi(argv[++i]);
            if (K < 2 || K > 6) {
                printf("curvemap: LUT size must be 2-6\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-P") == 0) {
            nPasses = atoi(argv[++i]);
            if (nPasses < 1) {
                printf("curvemap: pass count must be >= 1\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-h") == 0) {
            goto usage;
        } else {
            printf("curvemap: unknown argument %s\n", argv[i]);
            goto usage;
        }
    }

    {
        Abc_Ntk_t* pNtk = Abc_FrameReadNtk(pAbc);
        if (!pNtk) {
            printf("curvemap: empty network\n");
            return 0;
        }

        Abc_Ntk_t* pStrash = pNtk;
        if (!Abc_NtkIsStrash(pNtk)) {
            pStrash = Abc_NtkStrash(pNtk, 0, 0, 0);
            if (!pStrash) {
                printf("curvemap: strashing failed\n");
                return 1;
            }
        }

        Curvemap mapper(pStrash, K, nPasses);
        mapper.run();
        Abc_Ntk_t* pRes = mapper.mapped_ntk();
        if (!pRes) {
            printf("curvemap: mapping failed\n");
            return 1;
        }
        Abc_FrameReplaceCurrentNetwork(pAbc, pRes);
    }
    return 0;

usage:
    Abc_Print(-2, "\nusage: curvemap [-K <num>] [-P <num>]\n");
    Abc_Print(-2, "\t         area-delay Pareto-curve LUT mapper\n");
    Abc_Print(-2, "\t-K [int] : LUT input size (2-6) [default = 6]\n");
    Abc_Print(-2, "\t-P [int] : number of mapping passes (>=1) [default = 3]\n");
    Abc_Print(-2, "\t-h       : print the command usage\n");
    return 1;
}

struct CmdRegister
{
    CmdRegister()
    {
        Cmd_CommandAdd(Abc_FrameGetGlobalFrame(), "FoxSYN", "foxmap", Foxmap_Command, 1);
        Cmd_CommandAdd(Abc_FrameGetGlobalFrame(), "FoxSYN", "smap",   Suppermap_Command, 1);
        Cmd_CommandAdd(Abc_FrameGetGlobalFrame(), "FoxSYN", "partsyn", PartSyn_Command, 1);
        Cmd_CommandAdd(Abc_FrameGetGlobalFrame(), "FoxSYN", "hpart", HPart_Command, 1);
        Cmd_CommandAdd(Abc_FrameGetGlobalFrame(), "FoxSYN", "timer", Timer_Command, 1);
        Cmd_CommandAdd(Abc_FrameGetGlobalFrame(), "FoxSYN", "cpr", Cpr_Command, 1);
        Cmd_CommandAdd(Abc_FrameGetGlobalFrame(), "FoxSYN", "cmfs", Cmfs_Command, 1);
        Cmd_CommandAdd(Abc_FrameGetGlobalFrame(), "FoxSYN", "curvemap", Curvemap_Command, 1);
        Cmd_CommandAdd(Abc_FrameGetGlobalFrame(), "FPGA mapping", "agdmap", Agdmap, 1);
    }
} regiter;

int main(int argc, char *argv[])
{
    return ABC_NAMESPACE_PREFIX Abc_RealMain(argc, argv);
}
