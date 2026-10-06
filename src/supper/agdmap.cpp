
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
    uint8    cuts  [MAX_GATE_SIZE]{0};    // the idx of cut, in _sub_cuts vector
    uint8    numc  {0};                   // sub-cuts number
    uint8    ibins [AGD_MAX_LUT_SIZE]{0}; // the decomposition-tree inputs bins
    uint8    numb  {0};                   // input bins number
    uint8    inv   {0};                   // root is inverted or not
    Time     arr   {0};

    Bin() {}

    Inline uint  *leaf_begin() { return leaves;        }
    Inline uint8 *cut_begin () { return cuts;          }
    Inline uint8 *bin_begin () { return ibins;         }
    Inline uint  *leaf_end  () { return leaves + numl; }
    Inline uint8 *cut_end   () { return cuts   + numc; }
    Inline uint8 *bin_end   () { return ibins  + numb; }

    // A bin accumulates one entry per packed sub-cut, so cuts[] is indexed by
    // numc (bounded by the gate's fanin count) and not by its port count.
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

        uint  cut_len = sizeof(Cut) * _num + sizeof(uint) * (int)num_edge;
        uint  rid_len = sizeof(uint) * _num;
        uint  len     = cut_len + rid_len;
        Cut  *mem     = (Cut  *)std::calloc(1, len);
        char *ptr     = (char *)mem;
        char *cut_end = ptr + cut_len;

        uint *root_id_arr = reinterpret_cast<uint *>(cut_end);
        uint  cut_order   = 0;
        uint  num_virtual = 0;

        auto gen_cut_fn = [&](this auto self, agd_manager &agd_mgr, Bin &bin) -> uint {
            uint my_order = cut_order++;
            uint ms  = Cut::bytes_needed<Cut::data_t::KCUT>(bin.num_port());
            Cut *cut = (Cut *)ptr;
            ptr     += ms;

            if (ptr == cut_end) {
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
                const uint diff = reinterpret_cast<char *>(cut) - reinterpret_cast<char *>(mem); Assert(diff < cut_len);
                bin.root = AGD_MAX_ID + diff;
            }

            root_id_arr[my_order] = bin.root;

            Assert(cut->size <= AGD_MAX_LUT_SIZE);

            // Calculate the cut truth
            // For each sub-cut, gather its kCut representation, combine them one by one and get the truth.
            // sub_cuts.clear();
            kCut<AGD_MAX_LUT_SIZE> func;
            Time max_input_arr = 0;
            for (auto it = bin.cut_begin(); it != bin.cut_end(); ++it) {
                uint cut_idx = *it;
                Cut *icut = agd_mgr._sub_cuts[cut_idx];
                bool sign = agd_mgr._cut_roots[cut_idx].sign();
                func &= sign_cond(icut, sign);
                ForEachCutLeaf(icut) {
                    max_input_arr = std::max(max_input_arr, agd_mgr._mgr.arrival(leaf));
                }
            }

            for (uint i = 0; i != bin.numb; ++i) {
                Bin &fanin = agd_mgr._bins[bin.ibins[i]];
                kCut<1> var_cut(fanin.root, fanin.inv);
                func &= reinterpret_cast<Cut *>(&var_cut);
                max_input_arr = std::max(max_input_arr, fanin.arr);
            }

            // The leaf array built above (bin.leaves, then the fanin-bin roots)
            // can repeat a variable: a fanin bin holding a single cut contributes
            // that cut's own node id as its root, and the same node may already
            // be a leaf of another sub-cut packed into this bin. func combines
            // through kCut::operator&=, which dedups via set_union, so its
            // support can be smaller than cut->size. Expand func's truth to the
            // full port space (a repeated leaf is the same variable, so its
            // duplicate position copies the first occurrence) instead of
            // shrinking cut->size: num_bytes() is the stride used to walk the
            // cut chain, and that storage is sized by the port count.
            if (func.icut.size < cut->size) {
                const uint ports = cut->size;
                word padded = 0;
                for (uint row = 0; row < (1u << ports); ++row) {
                    uint fr = 0, fbit = 0;
                    for (uint j = 0; j < ports; ++j) {
                        if (j > 0 && cut->leaf(j) == cut->leaf(j - 1)) continue;
                        fr |= ((row >> j) & 1u) << fbit;
                        ++fbit;
                    }
                    if ((func.icut.fid() >> fr) & 1) padded |= (word)1 << row;
                }
                cut->set_fid(padded);
            } else {
                cut->set_fid(func.icut.fid());
            }

            if (bin.numc == 1 && bin.numb == 0) {
                bin.arr = agd_mgr._mgr.arrival(bin.root);
            } else {
                bin.arr = max_input_arr + 1;
            }
            return bin.root;
        };

        uint rid  = gen_cut_fn(*this, *root_bin); Assert(ptr == cut_end && rid == AGD_MAX_ID);
        Assert(cut_order == _num);
        _cost.arr = root_bin->arr;
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

        auto cut_arrival = [this](Cut *cut) -> Time {
            Time arr = 0;
            for (uint *it = cut->begin(); it != cut->end(); ++it) {
                arr = std::max(arr, _mgr.arrival(*it));
            }
            return arr + 1;
        };

        // -- Sort sub-cuts in delay-oriented order
        std::stable_sort(sub_cuts.begin(), sub_cuts.end(), [&](Cut *lhs, Cut *rhs) {
            const Time lhs_arr = cut_arrival(lhs);
            const Time rhs_arr = cut_arrival(rhs);
            if (lhs_arr != rhs_arr) {
                return lhs_arr < rhs_arr;
            }
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
                    bin->arr = std::max(bin->arr, cut_arrival(cut));
                    packed = true;
                    break;
                }
            }
            if (!packed) {
                Bin *bin = _bins + _num++;
                bin->add_cut(cut->sign, ii, cut->begin(), cut->end());
                bin->arr = cut_arrival(cut);
            }
            ++ii;
        }

        // clear the signature of bins
        for (int i = 0; i != _num; ++i)
            _bins[i].sign = 0;

        // -- Multi-level decomposition
        // Delay mode keeps lower-arrival bins earlier when tree construction ties on size.
        std::stable_sort(_bins, _bins + _num, [](const Bin &lhs, const Bin &rhs) {
            if (lhs.numl != rhs.numl) {
                return lhs.numl < rhs.numl;
            }
            return lhs.arr < rhs.arr;
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

        // General case: depth-bucket bin connection strategy
        // Group bins by arrival (depth proxy): shallow bins get connected into deeper bins
        std::map<Time, std::vector<int>> depth_map;
        for (int i = 0; i < _num; ++i) {
            depth_map[_bins[i].arr].push_back(i);
        }

        // Layer-by-layer connection: connect waiting bins into current-layer bins with spare slots
        std::deque<int> unconnected;
        for (auto &[depth, bin_indices] : depth_map) {
            // Try to absorb waiting bins into current-layer bins that have spare slots
            for (int sink_idx : bin_indices) {
                if (unconnected.empty()) break;
                Bin *sink = _bins + sink_idx;
                while (!sink->full(lut_size) && !unconnected.empty()) {
                    sink->add_bin_conn(unconnected.front());
                    unconnected.pop_front();
                }
            }
            // Current-layer bins are now unconnected (waiting for a deeper parent)
            for (int b : bin_indices) {
                unconnected.push_back(b);
            }
        }

        // Collect: should have at least 1 unconnected bin at end
        if (unconnected.size() == 1) {
            return create_map_solution(_bins + unconnected.front());
        }

        // Find the bin with fewest ports (num_port = numl + numb) among unconnected
        int min_idx = unconnected.front();
        for (int b : unconnected) {
            if (_bins[b].num_port() < _bins[min_idx].num_port()) min_idx = b;
        }
        Bin *min_bin = _bins + min_idx;

        // Overlying: if min_bin can absorb all others as children
        // Check using num_port() (numl + numb) to account for already-connected children
        int overlying_ports = (int)min_bin->num_port() + (int)unconnected.size() - 1;
        if (overlying_ports <= (int)lut_size) {
            for (int b : unconnected) {
                if (b != min_idx) min_bin->add_bin_conn(b);
            }
            return create_map_solution(min_bin);
        }

        // Direct fallback: chain unconnected bins under a new root
        Assert(_num + 1 < (int)kMaxBinNum);
        Bin *root = _bins + _num++;
        root->arr = _bins[unconnected.back()].arr + 1;
        while (!unconnected.empty()) {
            int front = unconnected.front();
            unconnected.pop_front();
            if (!root->full(lut_size)) {
                root->add_bin_conn(front);
            } else {
                Assert(_num + 1 < (int)kMaxBinNum);
                Bin *new_root = _bins + _num++;
                new_root->arr = root->arr + 1;
                new_root->add_bin_conn(front);
                new_root->add_bin_conn(root - _bins);
                root = new_root;
            }
        }
        return create_map_solution(root);
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
        cost.arr  = 0;
        for (uint *it = cut->begin(); it != cut->end(); ++it) {
            cost.arr = std::max(cost.arr, mgr.arrival(*it));
        }
        cost.arr += 1;
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
