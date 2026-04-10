#pragma once
#include <cstdint>
#include <string>

namespace itch {

struct InstrumentInfo {
    uint16_t    stock_locate{};
    std::string symbol{};
    char        trading_state{};
    uint32_t    round_lot_size{};
    char        market_category{};
    char        financial_status{};
};

} // namespace itch