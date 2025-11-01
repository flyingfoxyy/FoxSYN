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
    void add_input(Lit input) {
        _inputs.push_back(input);
    }
    const std::vector<Lit> &inputs() const {
        return _inputs;
    }

};


}
