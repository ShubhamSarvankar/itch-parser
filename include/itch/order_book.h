#pragma once
#include <cstdint>
#include <map>
#include <functional>
#include <optional>
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

    // Cached top of book. Empty when the side is empty. Updated by the
    // engine on insert / erase of the best level. Lets callers (notably
    // /book/:symbol/top) read top-of-book without touching the std::map.
    //
    // Note: holding raw pointers into a std::map is safe across non-erasing
    // operations because std::map does not invalidate iterators on insert.
    // The engine resets these on level erase.
    const PriceLevel* best_bid{nullptr};
    const PriceLevel* best_ask{nullptr};
};

} // namespace itch
