#pragma once

#include "basic.hpp"

namespace fox::supper {
enum class PMT {
    Unified,
    Separated
};

struct noop_dealloc {
    template<typename U>
    void operator()(U) const noexcept {} 
};

template <typename  T, PMT M, typename Dealloc = noop_dealloc>
class Prune {
    std::vector<std::vector<T>>    _separated_set;
    std::vector<T>                 _unified_set;
    std::vector<uint>              _capacity;
    std::function<bool(T, T)>      _cmp;
    Area                           _diff;

public:
    Prune(std::function<bool(T, T)> &&cmp) : _cmp(cmp), _diff(1.0) {}

    ~Prune() = default;

    Inline void set_diff(Area diff) {
        _diff = diff;
    }

    Inline void set_cap(std::vector<uint> &&list) {
        _capacity = std::move(list);
    }

    Inline void reset(std::size_t new_max_size, uint max_num = 4) {
        if constexpr (M == PMT::Unified) {
            _unified_set.clear();
            _unified_set.reserve(64);
        } else {
            _separated_set.clear();
            _separated_set.resize(new_max_size + 1);
            for (int i = 2; i != _separated_set.size(); ++i) {
                _separated_set[i].reserve(max_num);
            }
            _capacity.resize(new_max_size + 1, max_num);
        }
    }

    Inline void reset(std::vector<uint> &&cap) {
        // static_assert(M != PMT::Unified);
        _capacity = cap;
    }

    Inline void insert(T item) {
        if constexpr (M == PMT::Unified) {
            _unified_set.push_back(item);
            // Do not sort elements now, using nth element ranking when getting
            // std::ranges::sort(_unified_set, _cmp);
        } else {
            auto &vec = _separated_set[item->size];
            vec.push_back(item);
            // TODO: using self-implemented insert sorting for better performance
            std::ranges::sort(vec, _cmp);
            if (vec.size() > _capacity[item->size]) {
                if (std::is_pointer_v<T>) {
                    Dealloc{}(vec.back());
                }
                vec.pop_back();
            }
        }
    }

    void get(std::vector<T> &set, size_t n = 0)
    {
        set.reserve(n);
        if constexpr (M == PMT::Unified)
        {
            std::nth_element(
                _unified_set.begin(),
                _unified_set.begin() + n,
                _unified_set.end(),
                _cmp
            );
            std::copy(_unified_set.begin(), _unified_set.begin() + n, std::back_inserter(set));
            return;
        }
        else if constexpr (M == PMT::Separated) {
            Area last = kMaxArea;
            for (size_t sz = 2; sz != _separated_set.size(); ++sz)
            {
                auto &vec = _separated_set[sz];
                for (auto it = vec.rbegin(); it != vec.rend(); ++it) {
                    T    elem = *it;
                    Area curr = kMaxArea;
                    if constexpr (std::is_pointer_v<T>) {
                        curr = elem->area();
                    } else {
                        curr = elem.area();
                    }
                    if (curr > last + _diff) {
                        continue;
                    }
                    set.push_back(elem);
                    last = curr;
                }
            }
            std::ranges::reverse(set);
        } else {
            Assert(0);
        }
    }

    void print() const;
};
} // namespace supper