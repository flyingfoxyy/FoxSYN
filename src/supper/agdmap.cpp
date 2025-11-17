
#include <array>
#include <cstddef>
#include <cstdint>

#include "macros.hpp"
#include "map.hpp"

namespace fox::supper {
struct Order {
    std::vector<std::pair<uint, std::array<uint, AGD_MAX_LUT_SIZE>>> orders;
};

struct Bin {
    uint     sign  {0};
    uint     root  {0};
    uint     leaves[AGD_MAX_LUT_SIZE]{0}; // real cut leaves
    uint8    numl  {0};                   // leaf size
    uint8    cuts  [AGD_MAX_LUT_SIZE]{0}; // the sub-cut index
    uint8    numc  {0};                   // sub-cuts number
    uint8    bins  [AGD_MAX_LUT_SIZE]{0}; // the decomposition-tree inputs bins
    uint8    numb  {0};                   // input bins number
    uint8    idx   {std::numeric_limits<uint8>::max()};

    Bin() {}

    Bin(Cut *cut) : sign(cut->sign), root(0), numl(cut->size), numc(1) {
        cuts[0] = cut->idx;
    }

    uint  *leaf_begin() { return leaves;         }
    uint  *leaf_end  () { return leaves + numl;  }
    uint8 *cut_begin () { return cuts;           }
    uint8 *cut_end   () { return cuts + numc;    }
    uint8 *bin_begin () { return bins;           }
    uint8 *bin_end   () { return bins + numb;    }

    bool full(uint k) const {
        Assert (numl + numb  <= k);
        return (numl + numb) == k;
    }

    bool free(uint k) const {
        return (numl + numb) < k;
    }

    uint num_port() const {
        return numl + numb;
    }

    void add_cut(Cut *&cut, uint *begin, uint *end) {
        uint new_size = numl = end - begin;
        for (int i = 0; i != new_size; ++i)
            leaves[i] = begin[i];
        cuts[numc++] = cut->idx;
        sign |= cut->sign;
        cut = nullptr;
    }

    // Connect bin to this bin's free port
    void add_bin_conn(Bin *bin) {
        bins[numb++] = bin->idx;
    }

    // Convert decompostion tree into a cut-tree
    Cut *build_mapping_solution() {
        Cut *root = nullptr;
        return root;
    }
};

class AgdMgr {
    mapper  &_mgr ;
    CutCost &_cost;
    Cut     *_wcut;
    uint     _id  ;

    Area &get_area() { return _cost.area; }
    Edge &get_edge() { return _cost.edge; }

public:

    AgdMgr(mapper &mgr, Cut *wcut, CutCost &cost) : _mgr(mgr), _cost(cost), _wcut(wcut) {}

    ~AgdMgr() = default;

    // TODO: change bins to vector. pop_front is unnecessary
    Bin *build_trivial_tree(std::deque<Bin *> &bins, Bin *extra, uint k) {
        Bin *root = extra++;
        if (bins.size() <= k) {
            for (auto &bin : bins)
                root->add_bin_conn(bin);
            return root;
        } else {
            int num_pushed = 0;
            while (num_pushed++ < k) {
                Bin *bin = bins.front();
                bins.pop_front();
                Assert(root->full(k) == false);
                root->add_bin_conn(bin);
            }
            bins.push_back(root);
            return build_trivial_tree(bins, extra++, k);
        }
    }

    Cut *multilevel_decompose(Bin *bins, uint num_bin, uint k) {
        Bin *b0 = bins;
        uint num_used = 1;

        std::deque<Bin *> tree; tree.push_back(bins + num_used++);
        uint idx = 0;
        while (idx++ != tree.size() && num_used < num_bin) {
            Bin *root = tree[idx];
            int n = k - root->num_port(); Assert(n >= 0);
            for (int i = 0; i < n; i++) {
                Bin *leaf = bins + num_used++;
                root->add_bin_conn(leaf);
                // if (!leaf->full(k))
                tree.push_back(leaf);
                if (num_used == num_bin)
                    break;
            }
        }

        if (num_used == num_bin) { // all bins are used, good, just return is ok
            return b0->build_mapping_solution();
        }

        // There are some left bins need to be connected
        // Try to connect them with free ports of b0
        //! TODO: Use an extra bin, for smaller root cut ?
        while (!b0->full(k) && num_used < num_bin) {
            Bin *leaf = bins + num_used++;
            b0->add_bin_conn(leaf);
        }

        if (num_used == num_bin) {
            return b0->build_mapping_solution();
        }

        Assert(b0->full(k));

        std::deque<Bin *> remainder;
        for (uint i = num_used; i != num_bin; ++i) {
            remainder.push_back(bins + i);
        }
        std::array<Bin, 7> extra_bins;
        for (int i = 0; i != extra_bins.size(); ++i) {
            extra_bins[i].idx = i + num_bin;
        }

        Bin *root = build_trivial_tree(remainder, extra_bins.data(), k);

        const uint num_extra_used = root - extra_bins.data();
        get_area() += num_extra_used;

        return root->build_mapping_solution();
    }

    Cut *area_decompose() {
        // -- Collect the sub-cuts
        const std::vector<Lit>  &gate_inputs = _mgr.gate(_id)->inputs();
        cut_data_w              *data        = _wcut->wdata();

        std::array<Cut *, MAX_GATE_SIZE> sub_cuts;
        for (int i = 0; i != gate_inputs.size(); ++i) {
            const auto &in_cuts = _mgr.cut_set(gate_inputs[i]);
            sub_cuts[i] = in_cuts[data->sub_cuts[i]];
        }

        // -- Sort sub-cuts in given order
        std::ranges::sort(sub_cuts, [](Cut *lhs, Cut *rhs) { return lhs->size > rhs->size; });

        // -- Bin-packing
        std::array<Bin ,     MAX_GATE_SIZE> bins;
        std::array<uint, Cut::MAX_CUT_SIZE> buffer;

        uint lut_size = _mgr.config().lut_size;
        uint num_bin  = 0;

        for (int i = 0; i < sub_cuts.size(); ++i)
        {
            Cut *cut = sub_cuts[i];
            for (int k = 0; k != num_bin; ++k)
            {
                Bin *bin = &bins[k];
                if (bin->numl + cut->size > lut_size && popcount(bin->sign | cut->sign) > lut_size)
                    continue;
                uint *end = std::set_union(bin->leaf_begin(), bin->leaf_end(), cut->begin(), cut->end(), buffer.begin());
                if (end - buffer.begin() <= lut_size)
                {
                    bin->add_cut(cut, buffer.begin(), end);
                    break;
                }
            }
            if (cut) {
                new (&bins[num_bin++]) Bin(cut);
            }
        }

        get_area() = num_bin;

        // -- Multi-level decomposition
        std::sort(bins.begin(), bins.begin() + num_bin, [](const Bin &lhs, const Bin &rhs) {
            return lhs.numl < rhs.numl;
        });

        // Set the bin idx
        for (int i = 0; i != num_bin; ++i) {
            bins[i].idx = i;
        }

        // Build decompostion tree
        // Handle the simple cases at first
        if (num_bin == 1) {
            return bins[0].build_mapping_solution();
        } else if (num_bin == 2) {
            uint size = bins[0].numl + bins[1].numl;
            if (size == lut_size) {
                Bin root;
                root.add_bin_conn(&bins[0]);
                root.add_bin_conn(&bins[1]);
                ++get_area();
                return root.build_mapping_solution();
            } else {
                uint idx = bins[0].numl > bins[1].numl ? 1 : 0;
                bins[idx].add_bin_conn(&bins[1 - idx]);
                return bins[idx].build_mapping_solution();
            }
        }

        // General case
        Cut *root = multilevel_decompose(bins.data(), num_bin, lut_size);
        return root;
    }

    Cut *delay_decompose() {
        return NULL;
    }
};

Cut *
agd_decompose(mapper &mgr, Cut *wcut, CutCost &cost) {
    AgdMgr agd(mgr, wcut, cost);
    if (mgr.config().opt_target == Config::AREA)
        return agd.area_decompose();
    else
        return agd.delay_decompose();
}

}
