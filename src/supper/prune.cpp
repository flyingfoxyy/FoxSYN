#include <iostream>

#include "prune.hpp"
#include "cut.hpp"

namespace fox::supper {
template <typename  T, PMT M, typename Dealloc> void
Prune<T, M, Dealloc>::print() const {
    auto &ss = std::cout;
    ss << "Prune stats:\n";
    if constexpr (M == PMT::Unified) {
        ss << " unified set size : " << _unified_set.size();
        for (const T &item : _unified_set) {
            if constexpr (std::is_pointer_v<T>) {
                ss << "  " << **item << "\n";
            } else {
                ss << "  " << *item << "\n";
            }
        }
    } else {
        ss << " separated set size : " << _separated_set.size() - 1 << "\n";
        for (int i = 2; i != _separated_set.size(); ++i) {
            ss << "[" << i << "]\n";
            for (const T &item : _separated_set[i]) {
                if constexpr (std::is_pointer_v<T>) {
                    ss << "  " << **item << "\n";
                } else {
                    ss << "  " << *item << "\n";
                }
            }
        }
    }
}

template class Prune<Cut *, PMT::Unified  >;
template class Prune<Cut *, PMT::Separated>;
}
