
#include <array>
#include <cstddef>
#include <cstdlib>
#include <sstream>
#include <vector>
#include <map>

#include "map.hpp"

namespace fox::supper {
struct Order {
    std::vector<std::pair<uint, std::array<uint, AGD_MAX_LUT_SIZE>>> orders;
};

struct Bin {
    union {
        uint     sign {0};
        uint     root;
    };
    uint     leaves[AGD_MAX_LUT_SIZE]{0}; // real cut leaves
    uint8    numl  {0};                   // leaf size
    uint8    cuts  [AGD_MAX_LUT_SIZE]{0}; // the idx of cut, in _sub_cuts vector
    uint8    numc  {0};                   // sub-cuts number
    uint8    bins  [AGD_MAX_LUT_SIZE]{0}; // the decomposition-tree inputs bins
    uint8    numb  {0};                   // input bins number

    Bin() {}

    Bin(Cut *cut) : sign(cut->sign), numl(cut->size), numc(1) {
        ForEachCutLeaf(cut) {
            leaves[i] = leaf;
        }
        cuts[0] = cut->idx;
    }

    void operator*() {
        std::stringstream ss;
        ss << "Root " << root << ", ";
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
    Inline uint8 *cut_begin () { return cuts;          }
    Inline uint8 *bin_begin () { return bins;          }
    Inline uint  *leaf_end  () { return leaves + numl; }
    Inline uint8 *cut_end   () { return cuts   + numc; }
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

    Inline void add_cut(Sign cut_sign, uint idx, uint *begin, uint *end) {
        std::copy(begin, end, leaves);
        cuts[numc++] = idx;
        sign |= cut_sign;
        numl  = end - begin;
    }

    // Connect bin to this bin's free port
    // offset <---> _bins + offset
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
    CutCost   &_cost;

    std::vector<Cut *> _sub_cuts;
    // TODO: optmize here
    Lit _cut_roots[MAX_GATE_SIZE]{Lit(0, 0)}; // indexed by cut idx

    //mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm

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
    Cut *build_mapping_solution(Bin *root_bin) {
        // TODO: if a bin is just a wrapper of a single cut, reuse the cut directly.
        uint num_edge = 0;
        for (int i = 0; i != _num; ++i) {
            num_edge += _bins[i].num_port();
        }
        _cost.area = _num;
        _cost.edge = num_edge;

        const size_t mem_size = sizeof(Cut) * _num + sizeof(uint) * (int)num_edge;
        Cut  *mem = (Cut  *)std::calloc(1, mem_size);
        char *ptr = (char *)mem;
        char *end = ptr + mem_size;
        
        uint num_virtual = 0;

        std::function<uint(Bin &)> rec_fn = [&](Bin &bin) -> uint {
            uint ms  = Cut::bytes_needed<Cut::data_t::KCUT>(bin.num_port());
            Cut *cut = (Cut *)ptr;
            cut->ms  = ms;
            ptr     += ms;

            if (ptr == end) {
                cut->tail = 1;
            }

            for (auto it = bin.leaf_begin(); it != bin.leaf_end(); ++it) {
                cut->add_leaf(*it);
            }

            for (auto it = bin.bin_begin(); it != bin.bin_end(); ++it) {
                const uint root = rec_fn(_bins[*it]);
                cut->add_leaf(root);
            }

            Assert(cut->size == bin.num_port());

            uint prev = 0;
            ForEachCutLeaf(cut) {
                if (leaf < prev) [[unlikely]] {
                    std::sort(cut->begin(), cut->end());
                    break;
                } else {
                    prev = leaf;
                }
            }

            if (bin.numc == 1 && bin.numb == 0) {
                const uint root_id = _cut_roots[bin.cuts[0]].id(); Assert(root_id < VID);
                return root_id;
            } else {
                ++num_virtual;
                const uint diff = reinterpret_cast<char *>(cut) - reinterpret_cast<char *>(mem); Assert(diff < mem_size);
                return VID + (diff);
            }
        };

        uint rid  = rec_fn(*root_bin); Assert(ptr == end && rid == VID);
        mem->idx  = rid >= VID ? num_virtual - 1 : num_virtual;
        mem->head = 1;
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
    agd_decompose_mgr(mapper &mgr, uint id, Cut *wcut, CutCost &cost) : _mgr(mgr), _wcut(wcut), _id(id), _num(0), _cost(cost) {}

    ~agd_decompose_mgr() = default;

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
        uint buf[AGD_MAX_LUT_SIZE] {0};
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
                new (_bins + _num++) Bin(cut);
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

    Cut *delay_decompose() {
        return nullptr;
    }

    Inline uint num_bins() const {
        return _num;
    }
};

Cut *
agd_decompose(mapper &mgr, uint id, Cut *wcut, CutCost &cost) {
    // General k-cut, just return itself
    if (wcut->size <= mgr.config().lut_size) {
        Cut *cut = Cut::alloc_kcut(wcut->begin(), wcut->end(), wcut->sign);
        cost.area = 1;
        cost.edge = wcut->size;
        return cut;
    }

    agd_decompose_mgr agd(mgr, id, wcut, cost);

    Cut *root_kcut = nullptr;
    if (mgr.config().opt_target == Config::AREA)
        root_kcut = agd.area_decompose();
    else
        root_kcut = agd.delay_decompose();

    return root_kcut;
}

}
