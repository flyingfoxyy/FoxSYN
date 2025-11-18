#pragma once

#include "basic.hpp"

namespace fox::supper {

class Gate {
    std::vector<Lit> _inputs;

public:
    Gate(std::vector<Lit> &&cover) : _inputs(std::move(cover)) {}

    uint size() const {
        return _inputs.size();
    }

    Lit operator[](std::size_t idx) const {
        return _inputs[idx];
    }

    Lit input(std::size_t idx) const {
        return _inputs[idx];
    }

    const std::vector<Lit> &inputs() const {
        return _inputs;
    }

};

}

