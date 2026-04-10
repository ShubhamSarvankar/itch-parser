#pragma once
#include <cstdint>
#include <map>
#include <functional>
#include "price.h"

namespace itch {

struct PriceLevel {
    Price    price{};
    uint64_t total_shares{};
    uint32_t order_count{};
};

struct OrderBook {
    uint16_t stock_locate{};
    uint64_t last_update_timestamp{0};

    // Bids: highest price first
    std::map<Price, PriceLevel, std::greater<Price>> bids{};

    // Asks: lowest price first (default ascending)
    std::map<Price, PriceLevel> asks{};
};

} // namespace itch