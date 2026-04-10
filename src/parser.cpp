#include "parser.h"
#include <stdexcept>
#include <cstring>
#include <algorithm>

namespace itch {

// Wire read helpers — all assume big-endian source bytes

static uint16_t read_be16(const uint8_t* p) {
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(p[0]) << 8) | p[1]
    );
}

static uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) <<  8) |
            static_cast<uint32_t>(p[3]);
}

// Timestamp is 6 bytes on the wire — zero-extend to uint64_t explicitly.
// Never memcpy 8 bytes here: that would read 2 bytes past the timestamp
// field into the next field, corrupting both.
static uint64_t read_be48(const uint8_t* p) {
    return (static_cast<uint64_t>(p[0]) << 40) |
           (static_cast<uint64_t>(p[1]) << 32) |
           (static_cast<uint64_t>(p[2]) << 24) |
           (static_cast<uint64_t>(p[3]) << 16) |
           (static_cast<uint64_t>(p[4]) <<  8) |
            static_cast<uint64_t>(p[5]);
}

static uint64_t read_be64(const uint8_t* p) {
    return (static_cast<uint64_t>(p[0]) << 56) |
           (static_cast<uint64_t>(p[1]) << 48) |
           (static_cast<uint64_t>(p[2]) << 40) |
           (static_cast<uint64_t>(p[3]) << 32) |
           (static_cast<uint64_t>(p[4]) << 24) |
           (static_cast<uint64_t>(p[5]) << 16) |
           (static_cast<uint64_t>(p[6]) <<  8) |
            static_cast<uint64_t>(p[7]);
}

// Strip trailing spaces from a fixed-width alpha field.
static std::string read_alpha(const uint8_t* p, std::size_t len) {
    std::string s(reinterpret_cast<const char*>(p), len);
    auto last = s.find_last_not_of(' ');
    if (last == std::string::npos) return "";
    s.resize(last + 1);
    return s;
}

// Length guards

static void require(const MessageBuffer& buf, std::size_t needed,
                    const char* msg_type) {
    if (buf.size() < needed) {
        throw std::runtime_error(
            std::string("Parser: truncated ") + msg_type +
            " message: need " + std::to_string(needed) +
            " bytes, got " + std::to_string(buf.size())
        );
    }
}

// Per-type deserializers
// Byte offsets are from the start of the payload (after the length prefix).
// Offset 0 is the message type byte.

static StockDirectoryMsg parse_stock_directory(const MessageBuffer& buf) {
    require(buf, 39, "StockDirectory");
    const uint8_t* p = buf.data();
    StockDirectoryMsg m;
    m.stock_locate        = read_be16(p + 1);
    m.tracking_number     = read_be16(p + 3);
    m.timestamp           = read_be48(p + 5);
    m.stock               = read_alpha(p + 11, 8);
    m.market_category     = static_cast<char>(p[19]);
    m.financial_status    = static_cast<char>(p[20]);
    m.round_lot_size      = read_be32(p + 21);
    m.round_lots_only     = static_cast<char>(p[25]);
    m.issue_classification= static_cast<char>(p[26]);
    m.issue_sub_type      = read_alpha(p + 27, 2);
    m.authenticity        = static_cast<char>(p[29]);
    m.short_sale_threshold= static_cast<char>(p[30]);
    m.ipo_flag            = static_cast<char>(p[31]);
    m.luld_ref_price_tier = static_cast<char>(p[32]);
    m.etp_flag            = static_cast<char>(p[33]);
    m.etp_leverage_factor = read_be32(p + 34);
    m.inverse_indicator   = static_cast<char>(p[38]);
    return m;
}

static StockTradingActionMsg parse_trading_action(const MessageBuffer& buf) {
    require(buf, 25, "StockTradingAction");
    const uint8_t* p = buf.data();
    StockTradingActionMsg m;
    m.stock_locate    = read_be16(p + 1);
    m.tracking_number = read_be16(p + 3);
    m.timestamp       = read_be48(p + 5);
    m.stock           = read_alpha(p + 11, 8);
    m.trading_state   = static_cast<char>(p[19]);
    m.reserved        = static_cast<char>(p[20]);
    m.reason          = read_alpha(p + 21, 4);
    return m;
}

static AddOrderMsg parse_add_order(const MessageBuffer& buf) {
    require(buf, 36, "AddOrder");
    const uint8_t* p = buf.data();
    AddOrderMsg m;
    m.stock_locate    = read_be16(p + 1);
    m.tracking_number = read_be16(p + 3);
    m.timestamp       = read_be48(p + 5);
    m.order_ref       = read_be64(p + 11);
    m.side            = static_cast<char>(p[19]);
    m.shares          = read_be32(p + 20);
    m.stock           = read_alpha(p + 24, 8);
    m.price.raw       = read_be32(p + 32);
    return m;
}

static AddOrderMPIDMsg parse_add_order_mpid(const MessageBuffer& buf) {
    require(buf, 40, "AddOrderMPID");
    const uint8_t* p = buf.data();
    AddOrderMPIDMsg m;
    m.stock_locate    = read_be16(p + 1);
    m.tracking_number = read_be16(p + 3);
    m.timestamp       = read_be48(p + 5);
    m.order_ref       = read_be64(p + 11);
    m.side            = static_cast<char>(p[19]);
    m.shares          = read_be32(p + 20);
    m.stock           = read_alpha(p + 24, 8);
    m.price.raw       = read_be32(p + 32);
    m.attribution     = read_alpha(p + 36, 4);
    return m;
}

static OrderExecutedMsg parse_order_executed(const MessageBuffer& buf) {
    require(buf, 31, "OrderExecuted");
    const uint8_t* p = buf.data();
    OrderExecutedMsg m;
    m.stock_locate    = read_be16(p + 1);
    m.tracking_number = read_be16(p + 3);
    m.timestamp       = read_be48(p + 5);
    m.order_ref       = read_be64(p + 11);
    m.executed_shares = read_be32(p + 19);
    m.match_number    = read_be64(p + 23);
    return m;
}

static OrderExecutedPriceMsg parse_order_executed_price(const MessageBuffer& buf) {
    require(buf, 36, "OrderExecutedPrice");
    const uint8_t* p = buf.data();
    OrderExecutedPriceMsg m;
    m.stock_locate    = read_be16(p + 1);
    m.tracking_number = read_be16(p + 3);
    m.timestamp       = read_be48(p + 5);
    m.order_ref       = read_be64(p + 11);
    m.executed_shares = read_be32(p + 19);
    m.match_number    = read_be64(p + 23);
    m.printable       = static_cast<char>(p[31]);
    m.execution_price.raw = read_be32(p + 32);
    return m;
}

static OrderCancelMsg parse_order_cancel(const MessageBuffer& buf) {
    require(buf, 23, "OrderCancel");
    const uint8_t* p = buf.data();
    OrderCancelMsg m;
    m.stock_locate    = read_be16(p + 1);
    m.tracking_number = read_be16(p + 3);
    m.timestamp       = read_be48(p + 5);
    m.order_ref       = read_be64(p + 11);
    m.cancelled_shares= read_be32(p + 19);
    return m;
}

static OrderDeleteMsg parse_order_delete(const MessageBuffer& buf) {
    require(buf, 19, "OrderDelete");
    const uint8_t* p = buf.data();
    OrderDeleteMsg m;
    m.stock_locate    = read_be16(p + 1);
    m.tracking_number = read_be16(p + 3);
    m.timestamp       = read_be48(p + 5);
    m.order_ref       = read_be64(p + 11);
    return m;
}

static OrderReplaceMsg parse_order_replace(const MessageBuffer& buf) {
    require(buf, 35, "OrderReplace");
    const uint8_t* p = buf.data();
    OrderReplaceMsg m;
    m.stock_locate        = read_be16(p + 1);
    m.tracking_number     = read_be16(p + 3);
    m.timestamp           = read_be48(p + 5);
    m.original_order_ref  = read_be64(p + 11);
    m.new_order_ref       = read_be64(p + 19);
    m.shares              = read_be32(p + 27);
    m.price.raw           = read_be32(p + 31);
    return m;
}

// Dispatcher

std::optional<ParsedMessage> MessageParser::parse(const MessageBuffer& buf) {
    if (buf.empty()) {
        throw std::runtime_error("Parser: empty message buffer");
    }

    switch (static_cast<char>(buf[0])) {
        case 'R': return parse_stock_directory(buf);
        case 'H': return parse_trading_action(buf);
        case 'A': return parse_add_order(buf);
        case 'F': return parse_add_order_mpid(buf);
        case 'E': return parse_order_executed(buf);
        case 'C': return parse_order_executed_price(buf);
        case 'X': return parse_order_cancel(buf);
        case 'D': return parse_order_delete(buf);
        case 'U': return parse_order_replace(buf);
        default:  return std::nullopt;  // out-of-scope type, discard silently
    }
}

} // namespace itch