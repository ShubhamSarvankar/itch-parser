#pragma once
#include <cstdint>

namespace itch {

struct Price {
    uint32_t raw{0};

    double to_double() const { return raw / 10000.0; }

    auto operator<=>(const Price&) const = default;
};

} // namespace itch