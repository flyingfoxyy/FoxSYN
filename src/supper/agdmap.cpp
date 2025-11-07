
#include <algorithm>
#include <array>
#include <cstdint>

#include "macros.hpp"
#include "map.hpp"

namespace fox::supper {
struct Bin {
    uint     sign  {0};
    uint     root  {0};
    uint     leaves[MAX_LUT_SIZE]{0}; // real cut leaves
    uint8    size1 {0};               // leaf size
    uint8    cuts  [MAX_LUT_SIZE]{0}; // the sub-cut index
    uint8    size2 {0};               // sub-cuts number
    uint8    bins  [MAX_LUT_SIZE]{0}; // the decomposition-tree inputs bins
    uint8    size3 {0};               // input bins number
    uint8    idx;

    Bin();

    Bin(Cut *cut) : sign(cut->sign), root(0), size1(cut->size), size2(1) {
        cuts[0] = cut->idx;
    }

    uint *begin() { return leaves;         }
    uint *end()   { return leaves + size1; }

    void add_cut(Cut *&cut, uint *begin, uint *end) {
        size1 = end - begin;
        for (int i = 0; i != size1; ++i)
            leaves[i] = begin[i];
        cuts[size2++] = cut->idx;
        sign |= cut->sign;
        cut = nullptr;
    }

    void add_bin_conn(Bin *bin) {
        Assert(bin->idx);
        bins[size3++] = bin->idx;
    }

    uint num_port() const { return size1 + size3; }

    bool full(uint k) const {
        Assert(num_port() <= k);
        return num_port() == k;
    }

    // build decomposition tree, rooted at this bin
    Cut *build_mapping_solution() {
        Cut *cut = nullptr;

        return cut;
    }
};

template <bool AREA>
class AgdMgr {
    mapper  &_mgr ;
    CutCost &_cost;
    Cut     *_wcut;
    uint     _id  ;
public:

    AgdMgr(mapper &mgr, Cut *wcut, CutCost &cost) : _mgr(mgr), _cost(cost), _wcut(wcut) {}

    ~AgdMgr() = default;

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
        // Sort the bins by smaller -> bigger size order
        std::sort(bins, bins + num_bin, [](const Bin *lhs, const Bin *rhs) {
            return lhs->size1 < rhs->size1;
        });

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

        std::deque<Bin  *> remainder(bins + num_used, bins + num_bin);
        std::array<Bin, 7> extra_bins;
        for (int i = 0; i != extra_bins.size(); ++i) {
            extra_bins[i].idx = i + num_bin;
        }

        Bin *root = build_trivial_tree(remainder, extra_bins.data(), k);
        return root->build_mapping_solution();
    }

    Cut *decompose() {
        // -- Collect the sub-cuts
        const std::vector<Lit>          &gate_inputs = _mgr.gate(_id)->inputs();
        cut_data_u                      *data        = reinterpret_cast<cut_data_u *>(_wcut->leaves);
        std::array<Cut *, MAX_GATE_SIZE> sub_cuts;
        for (int i = 0; i != gate_inputs.size(); ++i) {
            const auto &in_cuts = _mgr.cut_set(gate_inputs[i]);
            sub_cuts[i] = in_cuts[data->sub_cuts[i]];
        }

        // -- Sort sub-cuts in given order
        if constexpr (AREA) {
            std::ranges::sort(sub_cuts, [](Cut *lhs, Cut *rhs) { return lhs->size > rhs->size; });
        } else {
            // std::ranges::sort(sub_cuts, [](Cut *lhs, Cut *rhs) { return lhs->num_bytes() < rhs->num_bytes(); });
        }

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
                if (bin->size1 + cut->size > lut_size && popcount(bin->sign | cut->sign) > lut_size)
                    continue;
                uint *end = std::set_union(bin->begin(), bin->end(), cut->begin(), cut->end(), buffer.begin());
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

        // -- Multi-level decomposition
        // uint total_bin_inputs = 0;
        // for (int i = 0; i != num_bin; ++i) {
        //     total_bin_inputs += bins[i].size1;
        // }

        // uint free_num = num_bin * lut_size - total_bin_inputs;

        // Sort the bins by smaller -> bigger size order
        // Or considering delay here ?
        std::ranges::sort(bins.begin(), bins.begin() + num_bin, [](const Bin &lhs, const Bin &rhs) {
            return lhs.size1 < rhs.size1;
        });

        // -- Connect the trees
        std::vector<Bin *> tree_roots;
        tree_roots.reserve(num_bin + 1);
        tree_roots.push_back(&bins[0]);
        uint idx = 0;
        uint pos = 0;
        while (idx != tree_roots.size()) {
            Bin *curr_root = tree_roots[idx];
            int free_in = lut_size - curr_root->size1;

            if (pos == num_bin)
                break;
        }

        Bin *root_bin = tree_roots.back();
        Cut *root_cut = root_bin->to_cut();
        return root_cut;
    }

    Area get_area();
    Edge get_edge();
    Time get_arrival();
};

Cut *
agd_decompose(mapper &mgr, Cut *wcut, CutCost &cost) {
    const bool area_first = mgr.config().opt_target == Config::AREA;
    if (area_first) {
        AgdMgr<true> dec_mgr(mgr, wcut, cost);
        Cut *cut  = dec_mgr.decompose();
        cost.area = dec_mgr.get_area();
        cost.edge = dec_mgr.get_edge();
        cost.arr  = dec_mgr.get_arrival();
        cost.size = cut->size;
        return cut;
    } else {
        AgdMgr<false> dec_mgr(mgr, wcut, cost);
        Cut *cut  = dec_mgr.decompose();
        cost.area = dec_mgr.get_area();
        cost.edge = dec_mgr.get_edge();
        cost.arr  = dec_mgr.get_arrival();
        cost.size = cut->size;
        return cut;
    }
}




}
