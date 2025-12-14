#include "base/abc/abc.h"
#include "misc/util/utilTruth.h"

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
    ss << "Area: " << a << ", Size: " << size << ", DT: " << (dt == 1 ? 'w' : 'k') << ", Leaves: ";
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

    if (s0)
        truth0 = ~truth0;
    if (s1)
        truth1 = ~truth1;

    return truth0 & truth1;
}

}
