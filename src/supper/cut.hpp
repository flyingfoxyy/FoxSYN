#pragma once

#include <vector>
#include <memory>
#include <cstdlib>

#include "basic.hpp"

namespace fox::supper {
struct LutLib
{
    std::vector<Area> area_cost { 0, 1.00, 1.00, 1.00, 1.00, 1.00, 1.00 };
    std::vector<Edge> edge_cost { 0, 1.00, 2.00, 3.00, 4.00, 5.00, 6.00 };
};

//==----------------------------------------------------------------==//
//                              Cut class                             //
//==----------------------------------------------------------------==//
template <typename T, typename G, typename L>
struct cut_t
{
    using elem_type = T;
    word   truth     ; // truth table
    Area   area  {0} ; // effective area / area-flow / exact area
    Edge   edge  {0} ; // edge
    Sign   sign  {0} ; // signature
    Time   arr   : 24; // cut root arrival time
    uint   size  :  8; // cut-size
    T      leaves[0];  // dynamic array

    // -- Functions
    cut_t() : truth(0xAAAAAAAAAAAAAAAA), arr(0), size(0) {}
    cut_t(const cut_t &cut)            = delete;
    cut_t(cut_t &&cut) noexcept        = delete;
    cut_t &operator=(const cut_t &cut) = delete;
    cut_t &operator=(cut_t &&cut) noexcept = delete;

    uint num_byte()     const { return sizeof(cut_t<T, G, L>) + sizeof(T) * size; }
    bool null()         const { return !size;                               }

    bool merge_cut(cut_t *lhs, cut_t *rhs, int lut_size);

    typename std::enable_if<!std::is_pointer<T>::value, Area>::type
        ref_mffc(const G& g, const L& lib);
    typename std::enable_if<std::is_pointer<T>::value, Area>::type
        ref_mffc(const L& lib);
    Edge rip_mffc(const G &graph, const L &lib);
};

template <typename T, typename G, typename L>
using Cut = cut_t<T, G, L>;

template <typename T, typename G, typename L>
bool cut_t<T, G, L>::merge_cut(cut_t<T, G, L> *lhs, cut_t<T, G, L> *rhs, int lut_size)
{
    // leaves.resize(kMaxLutSize);
    int nSize0 = lhs->size;
    int nSize1 = rhs->size;
    T *pC0 = lhs->leaves;
    T *pC1 = rhs->leaves;
    T *pC  = leaves;
    int i, k, c, s;
    // the case of the largest cut sizes
    if (nSize0 == lut_size && nSize1 == lut_size)
    {
        for (i = 0; i < nSize0; i++)
        {
            if (pC0[i] != pC1[i])
                return 0;
            pC[i] = pC0[i];
        }
        this->size = lut_size;
        return 1;
    }
    // compare two cuts with different numbers
    i = k = c = s = 0;
    if (nSize0 == 0)
        goto FlushCut1;
    if (nSize1 == 0)
        goto FlushCut0;
    while (1)
    {
        if (c == lut_size)
            return 0;
        if (pC0[i] < pC1[k])
        {
            pC[c++] = pC0[i++];
            if (i >= nSize0)
                goto FlushCut1;
        }
        else if (pC0[i] > pC1[k])
        {
            pC[c++] = pC1[k++];
            if (k >= nSize1)
                goto FlushCut0;
        }
        else
        {
            pC[c++] = pC0[i++]; k++;
            if (i >= nSize0)
                goto FlushCut1;
            if (k >= nSize1)
                goto FlushCut0;
        }
    }

FlushCut0:
    if (c + nSize0 > lut_size + i)
        return 0;
    while (i < nSize0)
        pC[c++] = pC0[i++];
    this->size = c;
    return 1;

FlushCut1:
    if (c + nSize1 > lut_size + k)
        return 0;
    while (k < nSize1)
        pC[c++] = pC1[k++];
    this->size = c;
    return 1;
}


template <typename T, typename G, typename L>
typename std::enable_if<!std::is_pointer<T>::value, Area>::type
cut_t<T, G, L>::ref_mffc(const G& g, const L& lib)
{
    Area area = lib.area_cost[size];
    for (int i = 0; i != size; ++i)
    {
        typename G::node_type n = g[leaves[i]];
        if (n->get_ref_num()++ > 0 || !n->is_logic())
            continue;
        area += n->get_best_cut()->ref_mffc(g, lib);
    }
    return area;
}

template <typename T, typename G, typename L>
typename std::enable_if<std::is_pointer<T>::value, Area>::type
cut_t<T, G, L>::ref_mffc(const L& lib)
{
    Area area = lib.area_cost[size];
    for (int i = 0; i != size; ++i)
    {
        area += leaves[i]->get_best_cut()->ref_mffc(lib);
    }
    return area;
}

template <typename T, typename G, typename L>
Edge cut_t<T, G, L>::rip_mffc(const G& g, const L& lib)
{
    Edge edge = lib.edge_cost[size];
    for (int i = 0; i != size; ++i)
    {
        typename G::node_type n = g[leaves[i]];
        assert(n->get_ref_num() > 0);
        if (--n->get_ref_num() > 0 || !n->is_logic())
            continue;
        edge += n->get_best_cut()->rip_mffc();
    }
    return edge;
}

// mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm

template <typename T, typename... Args>
static inline T* allocate(uint size, Args&&... args) {
    return new (std::malloc(sizeof(T) + sizeof(typename T::elem_type) * size)) T(std::forward<Args>(args)...);
}

template <typename T>
static inline void deallocate(T* elem) noexcept {
    assert(elem);
    elem->~T();
    std::free(elem);
}

}
