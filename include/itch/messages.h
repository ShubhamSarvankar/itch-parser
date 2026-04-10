#pragma once
#include <cstdint>
#include <string>
#include <variant>
#include "price.h"

namespace itch {

struct StockDirectoryMsg {
    uint16_t    stock_locate{};
    uint16_t    tracking_number{};
    uint64_t    timestamp{};
    std::string stock{};
    char        market_category{};
    char        financial_status{};
    uint32_t    round_lot_size{};
    char        round_lots_only{};
    char        issue_classification{};
    std::string issue_sub_type{};
    char        authenticity{};
    char        short_sale_threshold{};
    char        ipo_flag{};
    char        luld_ref_price_tier{};
    char        etp_flag{};
    uint32_t    etp_leverage_factor{};
    char        inverse_indicator{};
};

struct StockTradingActionMsg {
    uint16_t    stock_locate{};
    uint16_t    tracking_number{};
    uint64_t    timestamp{};
    std::string stock{};
    char        trading_state{};
    char        reserved{};
    std::string reason{};
};

struct AddOrderMsg {
    uint16_t    stock_locate{};
    uint16_t    tracking_number{};
    uint64_t    timestamp{};
    uint64_t    order_ref{};
    char        side{};
    uint32_t    shares{};
    std::string stock{};
    Price       price{};
};

struct AddOrderMPIDMsg {
    uint16_t    stock_locate{};
    uint16_t    tracking_number{};
    uint64_t    timestamp{};
    uint64_t    order_ref{};
    char        side{};
    uint32_t    shares{};
    std::string stock{};
    Price       price{};
    std::string attribution{};
};

struct OrderExecutedMsg {
    uint16_t stock_locate{};
    uint16_t tracking_number{};
    uint64_t timestamp{};
    uint64_t order_ref{};
    uint32_t executed_shares{};
    uint64_t match_number{};
};

struct OrderExecutedPriceMsg {
    uint16_t stock_locate{};
    uint16_t tracking_number{};
    uint64_t timestamp{};
    uint64_t order_ref{};
    uint32_t executed_shares{};
    uint64_t match_number{};
    char     printable{};
    Price    execution_price{};
};

struct OrderCancelMsg {
    uint16_t stock_locate{};
    uint16_t tracking_number{};
    uint64_t timestamp{};
    uint64_t order_ref{};
    uint32_t cancelled_shares{};
};

struct OrderDeleteMsg {
    uint16_t stock_locate{};
    uint16_t tracking_number{};
    uint64_t timestamp{};
    uint64_t order_ref{};
};

struct OrderReplaceMsg {
    uint16_t stock_locate{};
    uint16_t tracking_number{};
    uint64_t timestamp{};
    uint64_t original_order_ref{};
    uint64_t new_order_ref{};
    uint32_t shares{};
    Price    price{};
};

using ParsedMessage = std::variant<
    StockDirectoryMsg,
    StockTradingActionMsg,
    AddOrderMsg,
    AddOrderMPIDMsg,
    OrderExecutedMsg,
    OrderExecutedPriceMsg,
    OrderCancelMsg,
    OrderDeleteMsg,
    OrderReplaceMsg
>;

} // namespace itch

