
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <vector>
#include <map>

#include "basic.hpp"
#include "cut.hpp"
#include "map.hpp"

namespace fox::supper {
constexpr uint kMaxBinNum = MAX_GATE_SIZE + 7;

static bool sort_cut_leaves(Cut *cut) {
    uint prev = 0;
    ForEachCutLeaf(cut) {
        if (leaf < prev) [[unlikely]] {
            std::sort(cut->begin(), cut->end());
            return true;
        } else {
            prev = leaf;
        }
    }
    return false;
}

struct Bin {
    union {
        uint     sign {0};
        uint     root;
    };
    uint     leaves[AGD_MAX_LUT_SIZE]{0}; // real cut leaves
    uint8    numl  {0};                   // leaf size
    uint8    cuts  [AGD_MAX_LUT_SIZE]{0}; // the idx of cut, in _sub_cuts vector
    uint8    numc  {0};                   // sub-cuts number
    uint8    ibins [AGD_MAX_LUT_SIZE]{0}; // the decomposition-tree inputs bins
    uint8    numb  {0};                   // input bins number
    uint8    inv   {0};                   // root is inverted or not

    Bin() {}

    Inline uint  *leaf_begin() { return leaves;        }
    Inline uint8 *cut_begin () { return cuts;          }
    Inline uint8 *bin_begin () { return ibins;         }
    Inline uint  *leaf_end  () { return leaves + numl; }
    Inline uint8 *cut_end   () { return cuts   + numc; }
    Inline uint8 *bin_end   () { return ibins  + numb; }

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

    Inline void add_cut(Sign cut_sign, uint idx, uint *begin, uint *end) {
        std::copy(begin, end, leaves);
        cuts[numc++] = idx;
        sign |= cut_sign;
        numl  = end - begin;
    }

    // Connect bin to this bin's free port
    // offset <---> _bins + offset
    Inline void add_bin_conn(uint8 offset) {
        ibins[numb++] = offset;
    }
};

class agd_manager {
    Bin            _bins[kMaxBinNum] {}; // buffer of bins for fast acess
    mapper        &_mgr ;
    Cut           *_wcut;
    uint           _id  ;
    uint           _num ;
    CutCost       &_cost;

    std::vector<Cut *> _sub_cuts;
    // TODO: optimize here
    Lit _cut_roots[MAX_GATE_SIZE]{Lit(0, 0)}; // indexed by cut idx

    //mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm

    Bin *build_triv_tree(std::vector<Bin *> &bins, uint k, uint start) {
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
            return build_triv_tree(bins, k, start);
        }
    }

    // Building a mapping solution tree from bin decomposition.
    // For a bin, its bins represents the fanout edges from these bins.
    Cut *create_map_solution(Bin *root_bin) {
        // TODO: if a bin is just a wrapper of a single cut, reuse the cut directly.
        uint num_edge = 0;
        for (int i = 0; i != _num; ++i) {
            num_edge += _bins[i].num_port();
        }
        _cost.area = _num;
        _cost.edge = num_edge;

        uint  len = sizeof(Cut) * _num + sizeof(uint) * (int)num_edge;
        Cut  *mem = (Cut  *)std::calloc(1, len);
        char *ptr = (char *)mem;
        char *end = ptr + len;

        uint num_virtual = 0;

        auto gen_cut_fn = [&](this auto self, agd_manager &agd_mgr, Bin &bin) -> uint {
            uint ms  = Cut::bytes_needed<Cut::data_t::KCUT>(bin.num_port());
            Cut *cut = (Cut *)ptr;
            ptr     += ms;

            if (ptr == end) {
                cut->tail = 1;
            }

            cut->ms = ms;
            for (auto it = bin.leaf_begin(); it != bin.leaf_end(); ++it) {
                cut->add_leaf(*it);
            }
            for (auto it = bin.bin_begin(); it != bin.bin_end(); ++it) {
                const uint root_id = self(agd_mgr, agd_mgr._bins[*it]);
                cut->add_leaf(root_id);
            }
            cut->ms = 0;

            // Make sure leaves are ordered.
            sort_cut_leaves(cut);

            // There is only one cut for this bin, then the cut root shall be the bin root. (existing logic node)
            if (bin.numc == 1 && bin.numb == 0) {
                const Lit  root_lit = agd_mgr._cut_roots[bin.cuts[0]];
                const uint root_id  = root_lit.id(); Assert(root_id < VID);
                bin.root = root_id;
                bin.inv  = root_lit.sign();
            } else {
                ++num_virtual;
                const uint diff = reinterpret_cast<char *>(cut) - reinterpret_cast<char *>(mem); Assert(diff < len);
                bin.root = AGD_MAX_ID + diff;
            }

            Assert(cut->size <= AGD_MAX_LUT_SIZE);

            // Calculate the cut truth
            // For each sub-cut, gather its kCut representation, combine them one by one and get the truth.
            // sub_cuts.clear();
            kCut<AGD_MAX_LUT_SIZE> func;
            for (auto it = bin.cut_begin(); it != bin.cut_end(); ++it) {
                uint cut_idx = *it;
                Cut *icut = agd_mgr._sub_cuts[cut_idx];
                bool sign = agd_mgr._cut_roots[cut_idx].sign();
                func &= sign_cond(icut, sign);
            }

            for (uint i = 0; i != bin.numb; ++i) {
                const Bin &fanin = agd_mgr._bins[bin.ibins[i]];
                kCut<1> var_cut(fanin.root, fanin.inv);
                func &= reinterpret_cast<Cut *>(&var_cut);
            }
            cut->set_fid(func.icut.fid());
            // Verify
            // if constexpr (kDebugBuild) {
            //     ForEachCutLeaf(cut) {
            //         Assert(leaf == bf.vars[i]);
            //     }
            // }
            return bin.root;
        };

        uint rid  = gen_cut_fn(*this, *root_bin); Assert(ptr == end && rid == AGD_MAX_ID);
        mem->idx  = rid >= AGD_MAX_ID ? num_virtual - 1 : num_virtual;
        mem->head = 1;
        mem->ms   = len;
        return mem;
    }

    Cut *multilevel_decompose(uint k) {
        Bin *b0 = _bins; // Bin *b1 = _bins + 1;
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
            return create_map_solution(b0);
        }

        // There are some left bins need to be connected
        // Try to connect them with free ports of b0
        //! TODO: Use an extra bin, for smaller root cut ?
        while (!b0->full(k) && num_used < _num) {
            b0->add_bin_conn(num_used++);
        }

        if (num_used == _num) {
            return create_map_solution(b0);
        }

        Assert(b0->full(k));

        // All free ports are used, but there are still some unconnected bins.
        // Using trival tree to connect them.
        std::vector<Bin *> remainder; remainder.reserve(10);
        remainder.push_back(b0);
        for (uint i = num_used; i != _num; ++i) {
            remainder.push_back(_bins + i);
        }
        Bin *root = build_triv_tree(remainder, k, 0);
        Cut *cut  = create_map_solution(root);
        return cut;
    }

    std::string print_bin(Bin &bin);
    void print();

public:
    agd_manager(mapper &mgr, uint id, Cut *wcut, CutCost &cost) : _mgr(mgr), _wcut(wcut), _id(id), _num(0), _cost(cost) {}

    ~agd_manager() = default;

    Cut *area_decompose() {
        // -- Collect the sub-cuts
        const Gate *g = _mgr.gate(_id);
        std::vector<Cut *> &sub_cuts = _sub_cuts;
        sub_cuts.reserve(g->size());

        std::map<Cut *, Lit> cut2root;
        for (uint i = 0, sz = g->size(); i != sz; ++i) {
            const auto &in_cuts = _mgr.cut_set(g->input(i));
            Cut *cut = in_cuts[_wcut->get_sub_cut(i)];
            sub_cuts.push_back(cut);
            cut2root[cut] = g->input(i);
        }

        // -- Sort sub-cuts in given order
        std::sort(sub_cuts.begin(), sub_cuts.end(), [](Cut *lhs, Cut *rhs) {
            return lhs->size > rhs->size;
        });

        uint ii = 0;
        for (Cut *cut : sub_cuts) {
            _cut_roots[ii++] = cut2root[cut];
        }

        // -- Bin-packing
        uint buf[AGD_MAX_LUT_SIZE + AGD_MAX_LUT_SIZE] {0};
        uint lut_size = _mgr.config().lut_size;

        ii = 0;
        for (Cut *cut : sub_cuts)
        {
            bool packed = false;
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
                    bin->add_cut(cut->sign, ii, buf, end);
                    packed = true;
                    break;
                }
            }
            if (!packed) {
                Bin *bin = _bins + _num++;
                bin->add_cut(cut->sign, ii, cut->begin(), cut->end());
            }
            ++ii;
        }

        // clear the signature of bins
        for (int i = 0; i != _num; ++i)
            _bins[i].sign = 0;

        // -- Multi-level decomposition
        // Sort the bins with a size increasing order
        std::sort(_bins, _bins + _num, [](const Bin &lhs, const Bin &rhs) {
            return lhs.numl < rhs.numl;
        });

        // Build decomposition tree
        // Handle the simple cases at first
        if (_num == 1) {
            return create_map_solution(_bins);
        } else if (_num == 2) {
            uint size = _bins[0].numl + _bins[1].numl;
            if (size == lut_size * 2) {
                Bin *root = _bins + _num++;
                root->add_bin_conn(0);
                root->add_bin_conn(1);
                return create_map_solution(root);
            } else {
                uint idx  = _bins[0].numl > _bins[1].numl ? 1 : 0;
                Bin *root = _bins + idx;
                root->add_bin_conn(1 - idx);
                return create_map_solution(root);
            }
        }

        // Handle the case that all the bins are full
        if (_bins[0].full(lut_size)) [[unlikely]] {
            std::vector<Bin *> tmp_bins; tmp_bins.reserve(_num * 3);
            for (int i = 0; i != _num; ++i) {
                tmp_bins.push_back(_bins + i);
            }
            Bin *root = build_triv_tree(tmp_bins, lut_size, 0);
            Cut *cut  = create_map_solution(root);
            return cut;
        }

        // General case
        return multilevel_decompose(lut_size);
    }

    Cut *delay_decompose() {
        return nullptr;
    }
};

Cut *
agd_decompose(mapper &mgr, uint id, Cut *wcut, CutCost &cost) {
    // Wide cut is already k-feasible. No need to decompose.
    if (wcut->size <= mgr.config().lut_size) {
        Cut *cut  = Cut::alloc_kcut(wcut->begin(), wcut->end(), 0);
        uint sign = 0;
        ForEachCutLeaf(cut) {
            sign |= SIGNATURE(leaf);
        }
        cut->sign = sign;
        cost.area = 1;
        cost.edge = wcut->size;
        // Compute the cut truth
        Gate *gate = mgr.gate(id);
        kCut<AGD_MAX_LUT_SIZE> func;
        for (uint i = 0; i != wcut->num_sub_cuts(); ++i) {
            Lit  gin = gate->input(i);
            Cut *sub_cut = mgr.cut_set(gin)[wcut->get_sub_cut(i)];
            func &= sign_cond(sub_cut, gin.sign());
        }
        cut->set_fid(func.icut.fid());
        return cut;
    }

    agd_manager agd(mgr, id, wcut, cost);

    Cut *root_kcut = nullptr;
    if (mgr.config().opt_target == Config::AREA)
        root_kcut = agd.area_decompose();
    else
        root_kcut = agd.delay_decompose();

    return root_kcut;
}

std::string
agd_manager::print_bin(Bin &bin) {
    std::stringstream ss;
    ss << "* internal cuts\n";
    for (auto it = bin.cut_begin(); it != bin.cut_end(); ++it) {
        ss << **_sub_cuts[*it] << "\n";
    }
    ss << "* bins ";
    if (bin.numb == 0) {
        ss << "<none>";
    } else {
        for (auto it = bin.bin_begin(); it != bin.bin_end(); ++it) {
            ss << static_cast<uint>(*it) << " ";
        }
    }
    ss << "\n";
    return ss.str();
}

void
agd_manager::print() {
    std::cout << "Sub-cuts\n";
    for (int i = 0; i != _sub_cuts.size(); ++i) {
        std::cout << *(*_sub_cuts[i]) << "\n";
    }
    std::cout << "\nBins\n";
    for (int i = 0; i != _num; ++i) {
        std::cout << "bin " << i << ":\n";
        std::cout << print_bin(_bins[i]) << "\n";
    }
    std::cout << "\n";
}

} // namespace fox::supper
