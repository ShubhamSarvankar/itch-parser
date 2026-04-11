#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace itch {

struct PriceLevelSnapshot {
    double   price{};
    uint64_t total_shares{};
    uint32_t order_count{};
};

struct OrderBookSnapshot {
    std::string                   symbol{};
    char                          trading_state{};
    std::vector<PriceLevelSnapshot> bids{};  // descending price
    std::vector<PriceLevelSnapshot> asks{};  // ascending price
    uint64_t                      last_update_timestamp{};  // 0 = no book activity yet
    uint16_t                      stock_locate{};
    uint32_t                      round_lot_size{};         // 0 = unknown (placeholder instrument)
    char                          market_category{};        // 0 = unknown
};

struct SystemSnapshot {
    std::unordered_map<std::string, OrderBookSnapshot> books{};
    uint64_t messages_processed{};
    uint64_t snapshot_timestamp{};  // wall clock ms since epoch
    bool     pipeline_complete{false};
};

} // namespace itch