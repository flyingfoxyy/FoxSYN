#include "partsyn.hpp"
#include "base/abc/abc.h"
#include "base/main/mainInt.h"
#include <vector>
#include <random>
#include <algorithm>
#include <thread>
#include <iostream>

ABC_NAMESPACE_IMPL_START

namespace fox::partsyn {

// Helper structure to hold partition information
struct Partition {
    std::vector<Abc_Obj_t*> nodes;  // Nodes in this partition
    Abc_Ntk_t* pNtk;                 // Network for this partition

    Partition() : pNtk(nullptr) {}
    ~Partition() {
        if (pNtk) {
            Abc_NtkDelete(pNtk);
        }
    }
};

// Random partition: distribute nodes randomly across partitions
static std::vector<Partition> PartitionNetwork(Abc_Ntk_t* pNtk, int num_parts, bool verbose) {
    std::vector<Partition> partitions(num_parts);
    std::vector<Abc_Obj_t*> internal_nodes;

    // Collect all internal nodes (AND gates)
    Abc_Obj_t* pObj;
    int i;
    Abc_NtkForEachNode(pNtk, pObj, i) {
        internal_nodes.push_back(pObj);
    }

    if (verbose) {
        std::cout << "Total internal nodes: " << internal_nodes.size() << std::endl;
    }

    // Random shuffle and distribute
    std::random_device rd;
    std::mt19937 gen(rd());
    std::shuffle(internal_nodes.begin(), internal_nodes.end(), gen);

    // Distribute nodes to partitions
    for (size_t idx = 0; idx < internal_nodes.size(); ++idx) {
        int part_id = idx % num_parts;
        partitions[part_id].nodes.push_back(internal_nodes[idx]);
    }

    if (verbose) {
        for (int p = 0; p < num_parts; ++p) {
            std::cout << "Partition " << p << ": " << partitions[p].nodes.size() << " nodes" << std::endl;
        }
    }

    return partitions;
}

// Create a sub-network for a partition
static Abc_Ntk_t* CreatePartitionNetwork(Abc_Ntk_t* pNtk, Partition& part, bool verbose) {
    // For now, create a duplicate of the original network
    // In a more sophisticated implementation, we would extract only the relevant cone
    Abc_Ntk_t* pPartNtk = Abc_NtkDup(pNtk);
    part.pNtk = pPartNtk;

    if (verbose) {
        std::cout << "Created partition network with "
                  << Abc_NtkNodeNum(pPartNtk) << " nodes" << std::endl;
    }

    return pPartNtk;
}

// Optimize a single partition
static void OptimizePartition(Abc_Ntk_t*& pPartNtk, const Config& cfg, int part_id) {
    if (cfg.verbose) {
        std::cout << "Optimizing partition " << part_id << "..." << std::endl;
    }

    // Apply basic ABC optimization commands
    // Using ABC's internal optimization routines
    for (int round = 0; round < cfg.opt_rounds; ++round) {
        // Balance the network
        Abc_Ntk_t* pTemp = Abc_NtkBalance(pPartNtk, 0, 0, 1);
        if (pTemp) {
            Abc_NtkDelete(pPartNtk);
            pPartNtk = pTemp;
        }

        // Rewrite (modifies network in-place, returns status)
        Abc_NtkRewrite(pPartNtk, 0, 0, 0, 0, 0);

        // Refactor (returns status, modifies network in-place)
        Abc_NtkRefactor(pPartNtk, 10, 1, 16, 0, 0, 0, 0);
    }

    if (cfg.verbose) {
        std::cout << "Partition " << part_id << " optimized to "
                  << Abc_NtkNodeNum(pPartNtk) << " nodes" << std::endl;
    }
}

// Merge optimized partitions back into a single network
static Abc_Ntk_t* MergePartitions(Abc_Ntk_t* pOriginal, std::vector<Partition>& partitions, bool verbose) {
    if (verbose) {
        std::cout << "Merging " << partitions.size() << " partitions..." << std::endl;
    }

    // For initial implementation, we'll use a simple strategy:
    // Take the best partition (smallest node count) as the result
    // In a more sophisticated implementation, we would merge the optimizations

    int best_idx = 0;
    int min_nodes = INT_MAX;

    for (size_t i = 0; i < partitions.size(); ++i) {
        if (partitions[i].pNtk) {
            int node_count = Abc_NtkNodeNum(partitions[i].pNtk);
            if (node_count < min_nodes) {
                min_nodes = node_count;
                best_idx = i;
            }
        }
    }

    if (verbose) {
        std::cout << "Selected partition " << best_idx
                  << " with " << min_nodes << " nodes" << std::endl;
    }

    // Return a duplicate of the best partition
    Abc_Ntk_t* pResult = Abc_NtkDup(partitions[best_idx].pNtk);
    return pResult;
}

// Main entry point
Abc_Ntk_t* PerformPartSyn(Abc_Ntk_t* pNtk, const Config& cfg) {
    if (!pNtk) {
        std::cerr << "Error: Input network is NULL" << std::endl;
        return nullptr;
    }

    if (!Abc_NtkIsStrash(pNtk)) {
        std::cerr << "Error: Network must be in AIG form" << std::endl;
        return nullptr;
    }

    if (cfg.verbose) {
        std::cout << "Starting partition-based synthesis..." << std::endl;
        std::cout << "Number of partitions: " << cfg.num_parts << std::endl;
        std::cout << "Original network: " << Abc_NtkNodeNum(pNtk) << " nodes" << std::endl;
    }

    // Step 1: Partition the network
    auto partitions = PartitionNetwork(pNtk, cfg.num_parts, cfg.verbose);

    // Step 2: Create sub-networks for each partition
    for (auto& part : partitions) {
        CreatePartitionNetwork(pNtk, part, cfg.verbose);
    }

    // Step 3: Optimize partitions in parallel
    std::vector<std::thread> threads;
    for (size_t i = 0; i < partitions.size(); ++i) {
        threads.emplace_back([&, i]() {
            if (partitions[i].pNtk) {
                OptimizePartition(partitions[i].pNtk, cfg, i);
            }
        });
    }

    // Wait for all threads to complete
    for (auto& t : threads) {
        t.join();
    }

    // Step 4: Merge the optimized partitions
    Abc_Ntk_t* pResult = MergePartitions(pNtk, partitions, cfg.verbose);

    if (cfg.verbose && pResult) {
        std::cout << "Final network: " << Abc_NtkNodeNum(pResult) << " nodes" << std::endl;
    }

    return pResult;
}

} // namespace fox::partsyn

ABC_NAMESPACE_IMPL_END
