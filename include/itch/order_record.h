#pragma once
#include <cstdint>
#include <string>
#include "price.h"

namespace itch {

struct OrderRecord {
    uint64_t    order_ref{};
    uint16_t    stock_locate{};
    char        side{};
    uint32_t    shares{};
    Price       price{};
    std::string mpid{};
};

} // namespace itch