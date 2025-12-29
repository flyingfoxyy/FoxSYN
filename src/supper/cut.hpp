#pragma once

#include "macros.hpp"
#include "basic.hpp"
#include <cstddef>
#include <cstring>

namespace fox::supper {
// ========================================================================
// Cut data for wide cut
// ========================================================================
struct cut_data_w {
    uint8  sub_cuts[MAX_GATE_SIZE]; // sub-cut idx
    uint   nums;                    // num of sub-cut
    uint   leaves[0];               // cut leaves

    std::string operator*() const;
};

// ========================================================================
// Cut
// ========================================================================
class alignas(4) Cut {
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
    uint      tail :  1; // the last  cut in a cut-array
    uint      head :  1; // the first cut in a cut-array
    uint      dt   :  2; // data type
    uint      idx  : 10; // cut id, i.e., its position in cut list.
    uint      ms   : 11; // the number of bytes pointed by this cut
    uint      fid_h {0}; // truth table high part.
    uint      fid_l {0}; // truth table low  part.
private:
    uint      data[0];   // cut-data. Leaves or extended data.
public:

    Cut() : sign(0), size(0), tail(0), head(0), dt(0), idx(0), ms(0) {}

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

    Inline word fid() const {
        word result;
        static_assert(sizeof(result) == sizeof(fid_h) + sizeof(fid_l));
        std::memcpy(&result, &fid_h, sizeof(result));
        return result;
    }

    Inline void set_fid(word value) {
        std::memcpy(&fid_h, &value, sizeof(value));
    }

    Inline void flop_fid() {
        fid_h = ~fid_h;
        fid_l = ~fid_l;
    }

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
     * @brief Get the number of bytes needed by the cut.
     */
    Inline std::size_t num_bytes() const {
        return dt == data_t::KCUT ? bytes_needed<data_t::KCUT>(size) : bytes_needed<data_t::WCUT>(size);
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
        return dt == data_t::KCUT ? data : wdata()->leaves;
    }

    Inline const uint *begin() const {
        return dt == data_t::KCUT ? data : wdata()->leaves;
    }

    Inline uint *end() {
        return dt == data_t::KCUT ? data + size : wdata()->leaves + size;
    }

    Inline uint leaf(uint p) const { return begin()[p]; }

    Inline bool is_wcut() const { return dt == data_t::WCUT; }
    Inline bool is_kcut() const { return dt == data_t::KCUT; }

    // For cut copy
    static Inline Cut *copy(const Cut *cut) {
        uint num_bytes = cut->num_bytes();
        void *ptr = std::malloc(num_bytes);
        Cut *pcut = (Cut *)std::memcpy(ptr, cut, num_bytes);
        pcut->ms  = num_bytes;
        return pcut;
    }

    // Allocate a k-cut with given cut leaves. For general k-cut enumeration.
    static Inline Cut *alloc_kcut(uint *begin, uint *end, Sign s, Cut *cut = nullptr) {
        Cut *ptr = nullptr;
        if (cut) {
            ptr = cut;
        } else {
            const size_t num_bytes = bytes_needed<data_t::KCUT>(end - begin);
            ptr = static_cast<Cut *>(std::calloc(1, num_bytes));
            ptr->ms = num_bytes;
        }
        ptr->sign = s;
        ptr->size = end - begin;
        ptr->dt   = data_t::KCUT;
        std::memcpy(ptr->data, begin, sizeof(uint) * ptr->size);
        return ptr;
    }

    // Allocate a k-cut with pre-set leaf size
    static Inline Cut *alloc_kcut(uint leaf_size) {
        const size_t num_bytes = bytes_needed<data_t::KCUT>(leaf_size);
        Cut *ptr  = static_cast<Cut *>(std::calloc(1, num_bytes));
        ptr->size = 0;
        ptr->dt   = data_t::KCUT;
        ptr->ms   = num_bytes;
        return ptr;
    }

    // For trivial cut
    static Inline Cut *alloc_triv(uint id) {
        constexpr size_t num_bytes = bytes_needed<data_t::KCUT>(1);
        Cut *ptr  = static_cast<Cut *>(std::calloc(1, num_bytes));
        ptr->sign = SIGNATURE(id);
        ptr->size = 1;
        ptr->ms   = num_bytes;
        ptr->data[0] = id;
        return ptr;
    }

    // For Agdmap wide cut
    static Inline Cut *alloc_wcut(uint *begin, uint *end) {
        const size_t num_bytes = bytes_needed<data_t::WCUT>(end - begin);
        Cut *ptr  = static_cast<Cut *>(std::calloc(1, num_bytes));
        ptr->size = end - begin;
        ptr->dt   = data_t::WCUT;
        ptr->ms   = num_bytes;
        std::copy(begin, end, ptr->wdata()->leaves);
        return ptr;
    }

    static Inline void dealloc(Cut *cut) {
        // if ms is 0, this cut does not own a heap memory chunk.
        if (cut && cut->ms) [[likely]] {
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

    Inline uint num_sub_cuts() const {
        return wdata()->nums;
    }

    Inline Area &area() {
        return a;
    }

    Inline Cut *next() const {
        return tail ? nullptr : reinterpret_cast<Cut *>(reinterpret_cast<uintptr_t>(this) + num_bytes());
    }

    std::string operator*() const;
};

#define ForEachCutLeaf(C)                 \
    const auto _cut_begin = (C)->begin(); \
    for (uint leaf = 0, i = 0; i != (C)->size && (leaf = _cut_begin[i]); ++i)

// ========================================================================
// Special kCut for direct using.
// ========================================================================
template <std::size_t K>
struct kCut {
    Cut  icut      { };
    uint leaves[K] {0};

    kCut() {}

    kCut(uint var, bool inv) {
        icut.size = 1;
        icut.set_fid(inv ? 0x5555555555555555 : 0xAAAAAAAAAAAAAAAA);
        leaves[0] = var;
    }

    Inline kCut &operator=(const kCut &rhs) {
        if (&rhs == this)
            return *this;
        std::memcpy(this, &rhs, sizeof(rhs));
        return *this;
    }

    kCut &operator&=(Cut *rhs) {
        Cut *reg_ptr = regular(rhs);
        if (icut.size == 0) {
            std::memcpy(this, reg_ptr, reg_ptr->num_bytes());
            if (is_signed(rhs)) {
                icut.flop_fid();
            }
            return *this;
        }
        kCut<K> res;
        uint *end = std::set_union(icut.begin(), icut.end(), reg_ptr->begin(), reg_ptr->end(), res.icut.begin());
        res.icut.size = end - res.icut.begin();
        res.icut.set_fid(Cut::compute_truth(&res.icut, &icut, rhs, 0));
        return *this = res;
    }

    Inline void clear() {
        std::memset(this, 0, sizeof(kCut<K>));
    }
};

}
