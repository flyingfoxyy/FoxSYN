#include "base/abc/abc.h"
#include "basic.hpp"
#include "misc/util/utilTruth.h"
#include <algorithm>
#include <functional>

#include "cut.hpp"

namespace fox::supper {
std::string
Cut::operator*() const
{
    std::stringstream ss;
    ss << std::setprecision(4);
    if (is_kcut()) {
        ss << "K-Cut: ";
    } else {
        ss << "W-Cut: ";
    }
    if (dt == data_t::WCUT)
        ss << "Area: " << a << ",";
    else
        ss << "Sign: " << sign << ",";
    ss << " Size: " << size << ", DT: " << (dt == 1 ? 'w' : 'k') << ", Leaves: ";
    ss << "{ ";
    ForEachCutLeaf(this) {
        ss << leaf << " ";
    }
    ss << "}";
    if (is_wcut()) {
        ss << " " << **wdata();
    }
    return ss.str();
}

uint
Cut::compute_sign() const {
    uint sign = 0;
    ForEachCutLeaf(this) {
        sign |= SIGNATURE(leaf);
    }
    return sign;
}

word
Cut::compute_truth(const Cut *cut, const Cut *lhs, const Cut *rhs, int oper)
{
    const bool s0 = is_signed(lhs);
    const bool s1 = is_signed(rhs);
    auto TtExpand = [](word *pTruth, const Cut *sub, const Cut *cut) -> void
    {
        int i, k;
        for (i = cut->size - 1, k = sub->size - 1; i >= 0 && k >= 0; i--)
        {
            if (cut->leaf(i) > sub->leaf(k))
                continue;
            // assert(cut->leaves[i]->id() == sub->leaves[k]->id());
            if (k < i)
                Abc_TtSwapVars(pTruth, cut->size, k, i);
            k--;
        }
        assert( k == -1 );
    };

    word truth0 = lhs->fid;
    word truth1 = rhs->fid;

    TtExpand(&truth0, lhs, cut);
    TtExpand(&truth1, rhs, cut);

    if (s0) truth0 = ~truth0;
    if (s1) truth1 = ~truth1;

    return truth0 & truth1;
}

template <std::size_t K>
word compute_cut_truth(std::vector<kCut<K>> &sub_cuts) {
    using kcut = kCut<K>;
    std::function<kcut(const kcut&, const kcut&)> fn = [&](const kcut &lhs, const kcut &rhs) -> kcut {
        kcut res;
        auto end = std::set_union(lhs.leaves, lhs.leaves + lhs.icut.size, rhs.leaves, rhs.leaves + rhs.icut.size, res.leaves);
        res.icut.size = end - res.leaves;
        res.icut.fid  = Cut::compute_truth(res.raw_cut(), lhs.raw_cut(), rhs.raw_cut());
        return res;
    };

    if (sub_cuts.size() == 1) {
        return sub_cuts.front().icut.fid;
    }

    if (sub_cuts.size() == 2) {
        return fn(sub_cuts[0], sub_cuts[1]).icut.fid;
    }

    std::vector<kcut> tmp_cuts;
    tmp_cuts.reserve((sub_cuts.size() + 1) / 2);
    while (true) {
        tmp_cuts.clear();
        for (uint i = 0; i + 1 < sub_cuts.size(); i += 2) {
            const kcut res = fn(sub_cuts[i], sub_cuts[i + 1]);
            tmp_cuts.push_back(res);
        }
        if (sub_cuts.size() % 2 == 1) {
            tmp_cuts.push_back(sub_cuts.back());
        }
        if (tmp_cuts.size() == 1) {
            return tmp_cuts.front().icut.fid;
        }
        std::swap(sub_cuts, tmp_cuts);
    }
    return 0;
}

template word compute_cut_truth<AGD_MAX_LUT_SIZE>(std::vector<kCut<AGD_MAX_LUT_SIZE>> &sub_cuts);

}
