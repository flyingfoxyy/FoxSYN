#pragma once

#include "macros.hpp"
#include "basic.hpp"

namespace fox::supper {
// ========================================================================
// Cut
// ========================================================================
struct cut_data_w {
    uint8_t  sub_cuts[MAX_GATE_SIZE];
    uint     nums; // num of sub-cut
    uint     leaves[0];

    Inline std::string operator*() const {
        std::string str = "<";
        for (int i = 0; i != nums; ++i) {
            str += std::to_string(sub_cuts[i]);
            if (i != nums - 1)
                str += " ";
        }
        str += ">";
        return str;
    }
};

class Cut {
public:
    static constexpr std::size_t BIT_NUM_SIZE = 7;
    static constexpr std::size_t MAX_CUT_SIZE = (1 << BIT_NUM_SIZE) - 1;
    static constexpr std::size_t BIT_NUM_SIGN = std::numeric_limits<uint>::digits;

    enum data_t : uint8_t { KCUT = 0, WCUT };

    union {
        uint  sign;      // signature
        Area  a   ;      // area-cost. Used for wide-cut enumeration on-the-fly pruning
    };
    uint      size :  7; // cut-set size
    uint      crs  :  1; // compressed leaf array (first leaf -> id, diff21, diff32 ... )
    uint      tail :  1; // the last  cut in a cut-array
    uint      head :  1; // the first cut in a cut-array
    uint      dt   :  2; // data type
    uint      idx  : 10; // cut id, i.e., its position in cut list.
    uint      ms   : 11; // the number of bytes pointed by this cut
    word      fid;       // truth table (num. var <= 5) or functional id
private:
    uint      data[0];   // cut-data. Leaves or extended data.
public:

    Inline Cut &operator=(const Cut &cut) {
        if (&cut == this)
            return *this;
        Assert(ms >= cut.num_bytes());
        uint ms_tmp = ms;
        std::memset((void *)this, 0, ms_tmp);
        std::memcpy((void *)this, &cut, cut.num_bytes());
        ms = ms_tmp; // restore ms
        return *this;
    }

    /**
     * @brief Calculate the signature of the cut.
     * 
     * @return uint 
     */
    uint compute_sign() const;

    void change_leaf(uint idx, uint new_id) {
        begin()[idx] = new_id;
    }

    static word compute_truth(const Cut *cut, const Cut *lhs, const Cut *rhs, int oper = 0);

    template<data_t T>
    static constexpr std::size_t bytes_needed(uint size) {
        if constexpr (T == KCUT) {
            return sizeof(Cut) + sizeof(uint) * size;
        } else if constexpr (T == WCUT) {
            return sizeof(Cut) + sizeof(cut_data_w) + sizeof(uint) * size;
        } else {
            static_assert(T == KCUT || T == WCUT, "Invalid cut data type");
            return 0; // unreachable
        }
    }

    /**
     * @brief Get the number of bytes used by the cut.
     */
    Inline std::size_t num_bytes() const {
        if (dt == data_t::KCUT)
            return bytes_needed<data_t::KCUT>(size);
        else
            return bytes_needed<data_t::WCUT>(size);
    }

    Inline cut_data_w *wdata() {
        Assert(dt == data_t::WCUT);
        return reinterpret_cast<cut_data_w *>(data);
    }

    Inline cut_data_w *wdata() const {
        Assert(dt == data_t::WCUT);
        return const_cast<cut_data_w *>(reinterpret_cast<const cut_data_w *>(data));
    }

    Inline uint *begin() {
        switch (dt) {
        case data_t::KCUT:
            return data;
        case data_t::WCUT:
            return wdata()->leaves;
        }
        return nullptr;
    }

    Inline const uint *begin() const {
        switch (dt) {
        case data_t::KCUT:
            return data;
        case data_t::WCUT:
            return wdata()->leaves;
        }
        return nullptr;
    }

    // TODO: crs, end is unknown
    Inline uint *end() {
        switch (dt) {
        case data_t::KCUT:
            return data + size;
        case data_t::WCUT:
            return wdata()->leaves + size;
        }
        return nullptr;
    }

    Inline uint leaf(uint p) const { return begin()[p]; }

    Inline bool is_wcut() const { return dt == data_t::WCUT; }
    Inline bool is_kcut() const { return dt == data_t::KCUT; }

    // For cut copy
    static Inline Cut *copy(const Cut &cut) {
        const std::size_t num_bytes = cut.num_bytes();
        void *ptr = std::malloc(num_bytes);
        Cut *pcut = (Cut *)std::memcpy(ptr, &cut, num_bytes);
        pcut->ms = num_bytes; // restore ms
        return pcut;
    }

    // Allocate a k-cut with given cut leaves
    static Inline Cut *alloc_kcut(uint *begin, uint *end, Sign s, uint crs = 0) {
        const size_t num_bytes = bytes_needed<data_t::KCUT>(end - begin);
        Cut *ptr  = static_cast<Cut *>(std::calloc(1, num_bytes));
        ptr->sign = s;
        ptr->size = end - begin;
        ptr->crs  = crs;
        ptr->dt   = data_t::KCUT;
     // ptr->idx  = 0;
        ptr->ms   = num_bytes;
     // ptr->root = 0;
     // ptr->fid  = 0;
        std::copy(begin, end, ptr->data);
        return ptr;
    }

    // Allocate a k-cut with pre-set leaf size
    static Inline Cut *alloc_kcut(uint leaf_size) {
        const size_t num_bytes = bytes_needed<data_t::KCUT>(leaf_size);
        Cut *ptr  = static_cast<Cut *>(std::calloc(1, num_bytes));
     // ptr->sign = 0;
        ptr->size = 0;
     // ptr->crs  = 0;
        ptr->dt   = data_t::KCUT;
     // ptr->idx  = 0;
        ptr->ms   = num_bytes;
     // ptr->root = 0;
     // ptr->fid  = 0;
     // std::copy(begin(), end(), ptr->data);
        return ptr;
    }

    // For trivial cut
    static Inline Cut *alloc_triv(uint id) {
        constexpr size_t num_bytes = bytes_needed<data_t::KCUT>(1);
        Cut *ptr  = static_cast<Cut *>(std::calloc(1, num_bytes));
        ptr->sign = SIGNATURE(id);
        ptr->size = 1;
     // ptr->crs  = 0;
     // ptr->dt   = data_t::KCUT;
     // ptr->idx  = 0;
        ptr->ms   = num_bytes;
     // ptr->root = 0;
     // ptr->fid  = 0;
        ptr->data[0] = id;
        return ptr;
    }

    // For Agdmap wide cut
    static Inline Cut *alloc_wcut(uint *begin, uint *end) {
        const size_t num_bytes = bytes_needed<data_t::WCUT>(end - begin);
        Cut *ptr  = static_cast<Cut *>(std::calloc(1, num_bytes));
     // ptr->sign = 0;
        ptr->size = end - begin;
     // ptr->crs  = 0;
        ptr->dt   = data_t::WCUT;
     // ptr->idx  = 0;
        ptr->ms   = num_bytes;
     // ptr->root = 0;
     // ptr->fid  = 0;
        std::copy(begin, end, ptr->wdata()->leaves);
        return ptr;
    }

    static Inline void dealloc(Cut *cut) {
        if (cut) [[likely]] {
            std::free(cut);
        }
    }

    Inline void add_leaf(uint leaf) {
        Assert(is_kcut());
        Assert(ms >= bytes_needed<data_t::KCUT>(size + 1));
        data[size++] = leaf;
    }

    Inline void add_sub_cut(uint sub_cut_idx) {
        wdata()->sub_cuts[wdata()->nums++] = sub_cut_idx;
    }

    Inline uint8 get_sub_cut(uint idx) const {
        return wdata()->sub_cuts[idx];
    }

    Inline Area &area() {
        return a;
    }

    Inline Cut *next() const {
        return tail ? nullptr : reinterpret_cast<Cut *>(reinterpret_cast<uintptr_t>(this) + num_bytes());
    }

    std::string operator*() const;
};

#define ForEachCutLeaf(C) \
    for (uint leaf = 0, i = 0; i != (C)->size && (leaf = (C)->leaf(i)); ++i)

}