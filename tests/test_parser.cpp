#include <gtest/gtest.h>
#include <fstream>
#include <stdexcept>
#include <cstdint>
#include <vector>
#include "feed_reader.h"
#include "parser.h"

static const char* FIXTURE = "tests/fixtures/sample.bin";

// Feed reader tests (Milestone 3)

TEST(FeedReader, ReadsExpectedMessageCount) {
    itch::FileFeedReader reader(FIXTURE);
    int count = 0;
    while (reader.next_message().has_value()) { ++count; }
    EXPECT_EQ(count, 5);
}

TEST(FeedReader, MessageLengthsMatchFixture) {
    std::vector<uint16_t> expected_lens = {36, 40, 31, 19, 39};
    itch::FileFeedReader reader(FIXTURE);
    int i = 0;
    std::optional<itch::MessageBuffer> msg;
    while ((msg = reader.next_message()).has_value()) {
        ASSERT_LT(i, static_cast<int>(expected_lens.size()));
        EXPECT_EQ(msg->size(), expected_lens[i]);
        ++i;
    }
    EXPECT_EQ(i, 5);
}

TEST(FeedReader, FirstByteIsMessageType) {
    std::vector<uint8_t> expected_types = {'A', 'F', 'E', 'D', 'R'};
    itch::FileFeedReader reader(FIXTURE);
    int i = 0;
    std::optional<itch::MessageBuffer> msg;
    while ((msg = reader.next_message()).has_value()) {
        ASSERT_LT(i, static_cast<int>(expected_types.size()));
        EXPECT_EQ((*msg)[0], expected_types[i]);
        ++i;
    }
}

TEST(FeedReader, ReturnsNulloptAtEOF) {
    itch::FileFeedReader reader(FIXTURE);
    while (reader.next_message().has_value()) {}
    EXPECT_FALSE(reader.next_message().has_value());
}

TEST(FeedReader, ThrowsOnMissingFile) {
    EXPECT_THROW(
        itch::FileFeedReader("/nonexistent/path/file.bin"),
        std::runtime_error
    );
}

TEST(FeedReader, ThrowsOnTruncatedFrame) {
    const char* path = "/tmp/truncated.bin";
    {
        std::ofstream f(path, std::ios::binary);
        uint8_t len[2] = {0x00, 0x14};
        f.write(reinterpret_cast<char*>(len), 2);
        uint8_t payload[5] = {'A', 0, 0, 0, 0};
        f.write(reinterpret_cast<char*>(payload), 5);
    }
    itch::FileFeedReader reader(path);
    EXPECT_THROW(reader.next_message(), std::runtime_error);
}

TEST(FeedReader, EmptyFileReturnsNullopt) {
    const char* path = "/tmp/empty.bin";
    { std::ofstream f(path, std::ios::binary); }
    itch::FileFeedReader reader(path);
    EXPECT_FALSE(reader.next_message().has_value());
}

// Parser helpers — big-endian write helpers for constructing test buffers

static void write_be16(std::vector<uint8_t>& buf, size_t off, uint16_t v) {
    buf[off]   = (v >> 8) & 0xFF;
    buf[off+1] =  v       & 0xFF;
}

static void write_be32(std::vector<uint8_t>& buf, size_t off, uint32_t v) {
    buf[off]   = (v >> 24) & 0xFF;
    buf[off+1] = (v >> 16) & 0xFF;
    buf[off+2] = (v >>  8) & 0xFF;
    buf[off+3] =  v        & 0xFF;
}

static void write_be48(std::vector<uint8_t>& buf, size_t off, uint64_t v) {
    buf[off]   = (v >> 40) & 0xFF;
    buf[off+1] = (v >> 32) & 0xFF;
    buf[off+2] = (v >> 24) & 0xFF;
    buf[off+3] = (v >> 16) & 0xFF;
    buf[off+4] = (v >>  8) & 0xFF;
    buf[off+5] =  v        & 0xFF;
}

static void write_be64(std::vector<uint8_t>& buf, size_t off, uint64_t v) {
    buf[off]   = (v >> 56) & 0xFF;
    buf[off+1] = (v >> 48) & 0xFF;
    buf[off+2] = (v >> 40) & 0xFF;
    buf[off+3] = (v >> 32) & 0xFF;
    buf[off+4] = (v >> 24) & 0xFF;
    buf[off+5] = (v >> 16) & 0xFF;
    buf[off+6] = (v >>  8) & 0xFF;
    buf[off+7] =  v        & 0xFF;
}

// Write a space-padded alpha field
static void write_alpha(std::vector<uint8_t>& buf, size_t off,
                        const std::string& s, size_t width) {
    for (size_t i = 0; i < width; ++i) {
        buf[off + i] = (i < s.size())
            ? static_cast<uint8_t>(s[i])
            : static_cast<uint8_t>(' ');
    }
}

// Parser tests — Add Order (type 'A')

TEST(Parser, AddOrder_AllFields) {
    itch::MessageBuffer buf(36, 0);
    buf[0] = 'A';
    write_be16(buf,  1, 42);            // stock_locate
    write_be16(buf,  3, 7);             // tracking_number
    write_be48(buf,  5, 34200000000000ULL); // timestamp
    write_be64(buf, 11, 999888777ULL);  // order_ref
    buf[19] = 'B';                      // side
    write_be32(buf, 20, 500);           // shares
    write_alpha(buf, 24, "AAPL", 8);   // stock
    write_be32(buf, 32, 1894200);       // price raw

    itch::MessageParser parser;
    auto result = parser.parse(buf);
    ASSERT_TRUE(result.has_value());

    const auto& m = std::get<itch::AddOrderMsg>(*result);
    EXPECT_EQ(m.stock_locate,    42);
    EXPECT_EQ(m.tracking_number, 7);
    EXPECT_EQ(m.timestamp,       34200000000000ULL);
    EXPECT_EQ(m.order_ref,       999888777ULL);
    EXPECT_EQ(m.side,            'B');
    EXPECT_EQ(m.shares,          500u);
    EXPECT_EQ(m.stock,           "AAPL");
    EXPECT_EQ(m.price.raw,       1894200u);
}

TEST(Parser, AddOrder_SideSell) {
    itch::MessageBuffer buf(36, 0);
    buf[0]  = 'A';
    buf[19] = 'S';
    itch::MessageParser parser;
    auto result = parser.parse(buf);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<itch::AddOrderMsg>(*result).side, 'S');
}

TEST(Parser, AddOrder_SymbolTrailingSpacesStripped) {
    itch::MessageBuffer buf(36, 0);
    buf[0] = 'A';
    write_alpha(buf, 24, "MSFT", 8);  // "MSFT    " on wire
    itch::MessageParser parser;
    auto result = parser.parse(buf);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<itch::AddOrderMsg>(*result).stock, "MSFT");
}

TEST(Parser, AddOrder_PriceStoredAsRaw) {
    itch::MessageBuffer buf(36, 0);
    buf[0] = 'A';
    write_be32(buf, 32, 102500);
    itch::MessageParser parser;
    auto result = parser.parse(buf);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<itch::AddOrderMsg>(*result).price.raw, 102500u);
}

// Add Order with MPID (type 'F')

TEST(Parser, AddOrderMPID_AllFields) {
    itch::MessageBuffer buf(40, 0);
    buf[0] = 'F';
    write_be16(buf,  1, 10);
    write_be16(buf,  3, 1);
    write_be48(buf,  5, 12345678901ULL);
    write_be64(buf, 11, 111222333ULL);
    buf[19] = 'S';
    write_be32(buf, 20, 200);
    write_alpha(buf, 24, "GOOG", 8);
    write_be32(buf, 32, 500000);
    write_alpha(buf, 36, "NSDQ", 4);

    itch::MessageParser parser;
    auto result = parser.parse(buf);
    ASSERT_TRUE(result.has_value());

    const auto& m = std::get<itch::AddOrderMPIDMsg>(*result);
    EXPECT_EQ(m.stock_locate,    10);
    EXPECT_EQ(m.order_ref,       111222333ULL);
    EXPECT_EQ(m.side,            'S');
    EXPECT_EQ(m.shares,          200u);
    EXPECT_EQ(m.stock,           "GOOG");
    EXPECT_EQ(m.price.raw,       500000u);
    EXPECT_EQ(m.attribution,     "NSDQ");
}

// Order Executed (type 'E')

TEST(Parser, OrderExecuted_AllFields) {
    itch::MessageBuffer buf(31, 0);
    buf[0] = 'E';
    write_be16(buf,  1, 5);
    write_be16(buf,  3, 2);
    write_be48(buf,  5, 99999999999ULL);
    write_be64(buf, 11, 777666555ULL);
    write_be32(buf, 19, 100);
    write_be64(buf, 23, 888999ULL);

    itch::MessageParser parser;
    auto result = parser.parse(buf);
    ASSERT_TRUE(result.has_value());

    const auto& m = std::get<itch::OrderExecutedMsg>(*result);
    EXPECT_EQ(m.stock_locate,    5);
    EXPECT_EQ(m.tracking_number, 2);
    EXPECT_EQ(m.timestamp,       99999999999ULL);
    EXPECT_EQ(m.order_ref,       777666555ULL);
    EXPECT_EQ(m.executed_shares, 100u);
    EXPECT_EQ(m.match_number,    888999ULL);
}

// Order Executed with Price (type 'C')

TEST(Parser, OrderExecutedPrice_AllFields) {
    itch::MessageBuffer buf(36, 0);
    buf[0] = 'C';
    write_be16(buf,  1, 3);
    write_be16(buf,  3, 0);
    write_be48(buf,  5, 50000000000ULL);
    write_be64(buf, 11, 444333222ULL);
    write_be32(buf, 19, 50);
    write_be64(buf, 23, 12345ULL);
    buf[31] = 'Y';
    write_be32(buf, 32, 1900000);

    itch::MessageParser parser;
    auto result = parser.parse(buf);
    ASSERT_TRUE(result.has_value());

    const auto& m = std::get<itch::OrderExecutedPriceMsg>(*result);
    EXPECT_EQ(m.order_ref,          444333222ULL);
    EXPECT_EQ(m.executed_shares,    50u);
    EXPECT_EQ(m.match_number,       12345ULL);
    EXPECT_EQ(m.printable,          'Y');
    EXPECT_EQ(m.execution_price.raw,1900000u);
}

TEST(Parser, OrderExecutedPrice_PrintableN) {
    itch::MessageBuffer buf(36, 0);
    buf[0]  = 'C';
    buf[31] = 'N';
    itch::MessageParser parser;
    auto result = parser.parse(buf);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<itch::OrderExecutedPriceMsg>(*result).printable, 'N');
}

// Order Cancel (type 'X')

TEST(Parser, OrderCancel_AllFields) {
    itch::MessageBuffer buf(23, 0);
    buf[0] = 'X';
    write_be16(buf,  1, 8);
    write_be16(buf,  3, 3);
    write_be48(buf,  5, 11111111111ULL);
    write_be64(buf, 11, 555444333ULL);
    write_be32(buf, 19, 75);

    itch::MessageParser parser;
    auto result = parser.parse(buf);
    ASSERT_TRUE(result.has_value());

    const auto& m = std::get<itch::OrderCancelMsg>(*result);
    EXPECT_EQ(m.order_ref,        555444333ULL);
    EXPECT_EQ(m.cancelled_shares, 75u);
}

// Order Delete (type 'D')

TEST(Parser, OrderDelete_AllFields) {
    itch::MessageBuffer buf(19, 0);
    buf[0] = 'D';
    write_be16(buf,  1, 2);
    write_be16(buf,  3, 1);
    write_be48(buf,  5, 22222222222ULL);
    write_be64(buf, 11, 123456789ULL);

    itch::MessageParser parser;
    auto result = parser.parse(buf);
    ASSERT_TRUE(result.has_value());

    const auto& m = std::get<itch::OrderDeleteMsg>(*result);
    EXPECT_EQ(m.stock_locate, 2);
    EXPECT_EQ(m.order_ref,    123456789ULL);
}

// Order Replace (type 'U')

TEST(Parser, OrderReplace_AllFields) {
    itch::MessageBuffer buf(35, 0);
    buf[0] = 'U';
    write_be16(buf,  1, 15);
    write_be16(buf,  3, 4);
    write_be48(buf,  5, 33333333333ULL);
    write_be64(buf, 11, 111111111ULL);  // original_order_ref
    write_be64(buf, 19, 222222222ULL);  // new_order_ref
    write_be32(buf, 27, 350);           // shares
    write_be32(buf, 31, 501500);        // price

    itch::MessageParser parser;
    auto result = parser.parse(buf);
    ASSERT_TRUE(result.has_value());

    const auto& m = std::get<itch::OrderReplaceMsg>(*result);
    EXPECT_EQ(m.original_order_ref, 111111111ULL);
    EXPECT_EQ(m.new_order_ref,      222222222ULL);
    EXPECT_EQ(m.shares,             350u);
    EXPECT_EQ(m.price.raw,          501500u);
}

// Stock Directory (type 'R')

TEST(Parser, StockDirectory_AllFields) {
    itch::MessageBuffer buf(39, 0);
    buf[0] = 'R';
    write_be16(buf,  1, 1);
    write_be16(buf,  3, 0);
    write_be48(buf,  5, 1000000000ULL);
    write_alpha(buf, 11, "AAPL", 8);
    buf[19] = 'Q';   // market_category
    buf[20] = 'N';   // financial_status
    write_be32(buf, 21, 100);  // round_lot_size
    buf[25] = 'N';   // round_lots_only
    buf[26] = 'C';   // issue_classification
    write_alpha(buf, 27, "AD", 2);  // issue_sub_type
    buf[29] = 'P';   // authenticity
    buf[30] = 'N';   // short_sale_threshold
    buf[31] = 'N';   // ipo_flag
    buf[32] = '1';   // luld_ref_price_tier
    buf[33] = 'N';   // etp_flag
    write_be32(buf, 34, 0);  // etp_leverage_factor
    buf[38] = 'N';   // inverse_indicator

    itch::MessageParser parser;
    auto result = parser.parse(buf);
    ASSERT_TRUE(result.has_value());

    const auto& m = std::get<itch::StockDirectoryMsg>(*result);
    EXPECT_EQ(m.stock_locate,    1);
    EXPECT_EQ(m.stock,           "AAPL");
    EXPECT_EQ(m.market_category, 'Q');
    EXPECT_EQ(m.round_lot_size,  100u);
    EXPECT_EQ(m.authenticity,    'P');
}

// Stock Trading Action (type 'H')

TEST(Parser, TradingAction_TradingState_T) {
    itch::MessageBuffer buf(25, 0);
    buf[0]  = 'H';
    write_be16(buf, 1, 1);
    write_alpha(buf, 11, "AAPL", 8);
    buf[19] = 'T';
    write_alpha(buf, 21, "MWCB", 4);

    itch::MessageParser parser;
    auto result = parser.parse(buf);
    ASSERT_TRUE(result.has_value());

    const auto& m = std::get<itch::StockTradingActionMsg>(*result);
    EXPECT_EQ(m.trading_state, 'T');
    EXPECT_EQ(m.stock,         "AAPL");
    EXPECT_EQ(m.reason,        "MWCB");
}

TEST(Parser, TradingAction_AllStates) {
    itch::MessageParser parser;
    for (char state : {'T', 'H', 'P', 'Q'}) {
        itch::MessageBuffer buf(25, 0);
        buf[0]  = 'H';
        buf[19] = static_cast<uint8_t>(state);
        auto result = parser.parse(buf);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(std::get<itch::StockTradingActionMsg>(*result).trading_state,
                  state);
    }
}

// Unknown / out-of-scope type codes

TEST(Parser, OutOfScopeType_P_Discarded) {
    itch::MessageBuffer buf(10, 0);
    buf[0] = 'P';  // Trade (Non-Cross) — out of scope
    itch::MessageParser parser;
    EXPECT_FALSE(parser.parse(buf).has_value());
}

TEST(Parser, OutOfScopeType_B_Discarded) {
    itch::MessageBuffer buf(10, 0);
    buf[0] = 'B';  // Broken Trade — out of scope
    itch::MessageParser parser;
    EXPECT_FALSE(parser.parse(buf).has_value());
}

TEST(Parser, UnknownTypeByte_Discarded) {
    itch::MessageBuffer buf(10, 0);
    buf[0] = 0xFF;
    itch::MessageParser parser;
    EXPECT_FALSE(parser.parse(buf).has_value());
}

// Malformed input

TEST(Parser, EmptyBuffer_Throws) {
    itch::MessageBuffer buf;
    itch::MessageParser parser;
    EXPECT_THROW(parser.parse(buf), std::runtime_error);
}

TEST(Parser, TruncatedAddOrder_Throws) {
    itch::MessageBuffer buf(10, 0);  // AddOrder needs 36 bytes
    buf[0] = 'A';
    itch::MessageParser parser;
    EXPECT_THROW(parser.parse(buf), std::runtime_error);
}

// Timestamp zero-extension — 6-byte wire value must not bleed into next field

TEST(Parser, Timestamp_SixByteZeroExtension) {
    // Set timestamp to max 6-byte value: 0x0000FFFFFFFFFFFF
    // If parser incorrectly reads 8 bytes, it will also consume 2 bytes of
    // order_ref, corrupting both fields.
    itch::MessageBuffer buf(36, 0);
    buf[0] = 'A';
    // Write 0xFF into all 6 timestamp bytes
    for (int i = 0; i < 6; ++i) buf[5 + i] = 0xFF;
    // Write a known order_ref immediately after
    write_be64(buf, 11, 0x0102030405060708ULL);

    itch::MessageParser parser;
    auto result = parser.parse(buf);
    ASSERT_TRUE(result.has_value());

    const auto& m = std::get<itch::AddOrderMsg>(*result);
    EXPECT_EQ(m.timestamp, 0x0000FFFFFFFFFFFFULL);
    EXPECT_EQ(m.order_ref, 0x0102030405060708ULL);
}