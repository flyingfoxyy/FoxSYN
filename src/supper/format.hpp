#pragma once

#include <format>
#include <string>

namespace fox::supper {
static std::string formatted_float(double val, uint width, uint align) {
    return "";
    // return std::format("{:{}{}.2f}", val, align == 0 ? '>' : '<', width);
}


}
