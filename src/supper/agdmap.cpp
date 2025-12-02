
#include <array>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <vector>

#include "basic.hpp"
#include "macros.hpp"
#include "map.hpp"

constexpr uint VID = 1u << 31;

namespace fox::supper {
struct Order {
    std::vector<std::pair<uint, std::array<uint, AGD_MAX_LUT_SIZE>>> orders;
};

struct Bin {
    uint     sign  {0};
    uint     leaves[AGD_MAX_LUT_SIZE]{0}; // real cut leaves
    uint8    numl  {0};                   // leaf size
    uint8    cuts  [AGD_MAX_LUT_SIZE]{0}; // the sub-cut index
    uint8    numc  {0};                   // sub-cuts number
    uint8    bins  [AGD_MAX_LUT_SIZE]{0}; // the decomposition-tree inputs bins
    uint8    numb  {0};                   // input bins number
    uint8    idx   {std::numeric_limits<uint8>::max()}; // user-specified idx

    Bin() {}

    Bin(Cut *cut) : sign(cut->sign), numl(cut->size), numc(1) {
        ForEachCutLeaf(cut) {
            leaves[i] = leaf;
        }
        cuts[0] = cut->idx;
    }

    void operator*() {
        std::stringstream ss;
        ss << "Idx " << idx << ", ";
        ss << "Leaves {";
        for (auto it = leaf_begin(); it != leaf_end(); ++it) {
            ss << " " << *it;
        }
        ss << " }, ";
        ss << "Sub-cuts {";
        for (auto it = cut_begin(); it != cut_end(); ++it) {
            ss << " " << static_cast<uint>(*it);
        }
        ss << " }, ";
        ss << "Bins {";
        for (auto it = bin_begin(); it != bin_end(); ++it) {
            ss << " " << static_cast<uint>(*it);
        }
        ss << " }\n";
        std::cout << ss.str() << std::endl;
    }

    Inline uint  *leaf_begin() { return leaves;        }
    Inline uint  *leaf_end  () { return leaves + numl; }
    Inline uint8 *cut_begin () { return cuts;          }
    Inline uint8 *cut_end   () { return cuts   + numc; }
    Inline uint8 *bin_begin () { return bins;          }
    Inline uint8 *bin_end   () { return bins   + numb; }

    Inline bool full(uint k) const {
        Assert (numl + numb  <= k);
        return (numl + numb) == k;
    }

    Inline bool free(uint k) const {
        return (numl + numb) < k;
    }

    Inline uint num_port() const {
        return numl + numb;
    }

    Inline uint offset(Bin *start) const {
        return this - start;
    }

    Inline void add_cut(Cut *&cut, uint *begin, uint *end) {
        Assert(cut->is_kcut());
        std::copy(begin, end, leaves);
        cuts[numc++] = cut->idx;
        sign |= cut->sign;
        numl  = end - begin;
        cut   = nullptr;
    }

    // Connect bin to this bin's free port
    Inline void add_bin_conn(uint8 offset) {
        bins[numb++] = offset;
    }
};

class agd_decompose_mgr {
    static constexpr uint kMaxBinNum = MAX_GATE_SIZE + 7;
    Bin        _bins[kMaxBinNum] {}; // buffer of bins for fast acess
    mapper    &_mgr ;
    Cut       *_wcut;
    uint       _id  ;
    uint       _num ;

    Bin *build_trivial_tree(std::vector<Bin *> &bins, uint k, uint start) {
        Bin *root = _bins + _num++;
        if (bins.size() - start <= k) {
            for (uint i = start; i != bins.size(); ++i) {
                const Bin *bin = bins[i];
                root->add_bin_conn(bin->offset(_bins));
                Assert(root->num_port() <= k);
            }
            return root;
        } else {
            while (!root->full(k)) {
                const Bin *bin = bins[start++];
                root->add_bin_conn(bin->offset(_bins));
            }
            Assert(root->full(k));
            bins.push_back(root);
            return build_trivial_tree(bins, k, start);
        }
    }

    // Building a mapping solution tree from bin decomposition.
    // For a bin, its bins represents the fanout edges from these bins.
    std::vector<Cut *> build_mapping_solution(Bin *root) {
        // Assign the bins' idx in a dfs order
        uint count = 0;
        std::function<void(Bin &)> recursive_visit = [&](Bin &bin) {
            for (auto it = bin.bin_begin(); it != bin.bin_end(); ++it) {
                Bin &leaf = _bins[*it];
                recursive_visit(leaf);
            }
            bin.idx = count++;
        };
        
        recursive_visit(*root); Assert(root->idx == _num - 1 && count == _num);

        // Create k-cut for each bin
        std::vector<Cut *> cuts; cuts.resize(_num, nullptr);
        for (int i = 0; i != _num; ++i) {
            Bin &bin = _bins[i];
            // TODO: using mem pool here or using fixed-LUT-size cut object ?
            Cut *cut = Cut::alloc_kcut(bin.num_port());
            for (auto it = bin.leaf_begin(); it != bin.leaf_end(); ++it) {
                cut->add_leaf(*it);
            }
            for (auto it = bin.bin_begin(); it != bin.bin_end(); ++it) {
                uint idx = _bins[*it].idx;
                cut->add_leaf(idx + VID);
            }
            cuts[bin.idx] = cut;
        }

        return cuts;
    }

    std::vector<Cut *> multilevel_decompose(uint k) {
        Bin *b0 = _bins;
        // Bin *b1 = _bins + 1;
        b0->add_bin_conn(1);

        std::deque<uint> tree; tree.push_back(1); // tree has a initial root b1.

        uint num_used = 2;
        auto it = tree.begin();
        while (it != tree.end() && num_used < _num) {
            Bin &root = _bins[*it];
            while (root.free(k) && num_used < _num) {
                const uint leaf_offset = num_used++;
                root.add_bin_conn(leaf_offset);
                tree.push_back   (leaf_offset);
            }
            ++it;
        }

        if (num_used == _num) { // all bins are used, good, just return is ok
            return build_mapping_solution(b0);
        }

        // There are some left bins need to be connected
        // Try to connect them with free ports of b0
        //! TODO: Use an extra bin, for smaller root cut ?
        while (!b0->full(k) && num_used < _num) {
            b0->add_bin_conn(num_used++);
        }

        if (num_used == _num) {
            return build_mapping_solution(b0);
        }

        Assert(b0->full(k));

        // All free ports are used, but there are still some unconnected bins.
        // Using trival tree to connect them.
        std::vector<Bin *> remainder; remainder.reserve(10);
        for (uint i = num_used; i != _num; ++i) {
            remainder.push_back(_bins + i);
        }
        Bin *root = build_trivial_tree(remainder, k, 0);
        return build_mapping_solution(root);
    }

public:
    agd_decompose_mgr(mapper &mgr, uint id, Cut *wcut) : _mgr(mgr), _wcut(wcut), _id(id), _num(0) {}

    ~agd_decompose_mgr() = default;

    std::vector<Cut *> area_decompose() {
        // -- Collect the sub-cuts
        const Gate *g = _mgr.gate(_id);
        std::vector<Cut *> sub_cuts; sub_cuts.reserve(g->size());

        for (uint i = 0; i != g->size(); ++i) {
            const auto &in_cuts = _mgr.cut_set(g->input(i));
            sub_cuts.push_back(in_cuts[_wcut->get_sub_cut(i)]);
        }

        // -- Sort sub-cuts in given order
        std::sort(sub_cuts.begin(), sub_cuts.end(), [](Cut *lhs, Cut *rhs) {
            return lhs->size > rhs->size;
        });

        // -- Bin-packing
        uint buf[AGD_MAX_LUT_SIZE] {0};
        uint lut_size = _mgr.config().lut_size;

        for (Cut *cut : sub_cuts)
        {
            for (int k = 0; k != _num; ++k)
            {
                Bin *bin = _bins + k;
                if (bin->full(lut_size))
                    continue;
                if (bin->numl + cut->size > lut_size && popcount(bin->sign | cut->sign) > lut_size)
                    continue;
                uint *end = std::set_union(bin->leaf_begin(), bin->leaf_end(), cut->begin(), cut->end(), buf);
                if (end - buf <= lut_size)
                {
                    bin->add_cut(cut, buf, end);
                    break;
                }
            }
            if (cut) {
                new (_bins + _num++) Bin(cut);
            }
        }

        std::vector<Cut *>().swap(sub_cuts);

        // -- Multi-level decomposition
        // Sort the bins with a size increasing order
        std::sort(_bins, _bins + _num, [](const Bin &lhs, const Bin &rhs) {
            return lhs.numl < rhs.numl;
        });

        // Assign the bin idx (only the top-_num bins order changed)
        // for (int i = 0; i != kMaxBinNum; ++i)
        //     _bins[i].idx = i;

        // Build decomposition tree
        // Handle the simple cases at first
        if (_num == 1) {
            return build_mapping_solution(_bins);
        }
        else if (_num == 2) {
            uint size = _bins[0].numl + _bins[1].numl;
            if (size == lut_size) {
                Bin *root = _bins + _num++;
                root->add_bin_conn(0);
                root->add_bin_conn(1);
                return build_mapping_solution(root);
            } else {
                uint idx  = _bins[0].numl > _bins[1].numl ? 1 : 0;
                Bin *root = _bins + idx;
                root->add_bin_conn(1 - idx);
                return build_mapping_solution(root);
            }
        }

        // General case
        return multilevel_decompose(lut_size);
    }

    std::vector<Cut *> delay_decompose() {
        return {};
    }

    Inline uint num_bins() const {
        return _num;
    }
};

std::vector<Cut *>
agd_decompose(mapper &mgr, uint id, Cut *wcut) {
    // General k-cut, just return itself
    if (wcut->size <= mgr.config().lut_size) {
        Cut *cut = Cut::alloc_kcut(wcut->begin(), wcut->end(), wcut->sign);
        return {cut};
    }

    agd_decompose_mgr agd(mgr, id, wcut);
    std::vector<Cut *> dec_tree;

    if (mgr.config().opt_target == Config::AREA)
        dec_tree = agd.area_decompose();
    else
        dec_tree = agd.delay_decompose();

    // root k-cut locates in dec_tree + agd.num_bins() - 1.
    return dec_tree;
}

}
