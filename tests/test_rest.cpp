#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <cstdint>
#include <vector>
#include <thread>
#include <chrono>
#include "feed_reader.h"
#include "parser.h"
#include "book_engine.h"
#include "snapshot_publisher.h"
#include "rest_server.h"

using json = nlohmann::json;

// ---- write helpers (same pattern as test_parser.cpp) ----

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
static void write_alpha(std::vector<uint8_t>& buf, size_t off,
                        const std::string& s, size_t width) {
    for (size_t i = 0; i < width; ++i)
        buf[off + i] = (i < s.size()) ? static_cast<uint8_t>(s[i]) : ' ';
}

// Write a length-prefixed ITCH frame to a file
static void write_frame(std::ofstream& f, const std::vector<uint8_t>& payload) {
    uint16_t len = static_cast<uint16_t>(payload.size());
    uint8_t  hdr[2] = { static_cast<uint8_t>(len >> 8),
                         static_cast<uint8_t>(len & 0xFF) };
    f.write(reinterpret_cast<char*>(hdr), 2);
    f.write(reinterpret_cast<const char*>(payload.data()),
            static_cast<std::streamsize>(payload.size()));
}

// ---- Fixture message builders ----
// Offsets follow the ITCH 5.0 spec exactly.
// Prices are raw fixed-point (4 implied decimals):
//   287.16 = 2871600,  287.15 = 2871500
//   287.20 = 2872000,  287.21 = 2872100

static std::vector<uint8_t> make_stock_directory(uint16_t locate,
                                                  const std::string& symbol) {
    std::vector<uint8_t> buf(39, 0);
    buf[0] = 'R';
    write_be16(buf,  1, locate);
    write_be16(buf,  3, 0);
    write_be48(buf,  5, 0ULL);
    write_alpha(buf, 11, symbol, 8);
    buf[19] = 'Q';   // market_category
    buf[20] = 'N';   // financial_status
    write_be32(buf, 21, 100);  // round_lot_size
    buf[25] = 'N';
    buf[26] = 'C';
    write_alpha(buf, 27, "AD", 2);
    buf[29] = 'P';
    buf[30] = 'N';
    buf[31] = 'N';
    buf[32] = '1';
    buf[33] = 'N';
    write_be32(buf, 34, 0);
    buf[38] = 'N';
    return buf;
}

static std::vector<uint8_t> make_add_order(uint16_t locate, uint64_t ref,
                                            char side, uint32_t shares,
                                            uint32_t price_raw, uint64_t ts) {
    std::vector<uint8_t> buf(36, 0);
    buf[0] = 'A';
    write_be16(buf,  1, locate);
    write_be16(buf,  3, 0);
    write_be48(buf,  5, ts);
    write_be64(buf, 11, ref);
    buf[19] = static_cast<uint8_t>(side);
    write_be32(buf, 20, shares);
    // stock field at offset 24 — not needed for book engine (uses locate)
    write_be32(buf, 32, price_raw);
    return buf;
}

static std::vector<uint8_t> make_order_delete(uint16_t locate, uint64_t ref,
                                               uint64_t ts) {
    std::vector<uint8_t> buf(19, 0);
    buf[0] = 'D';
    write_be16(buf,  1, locate);
    write_be16(buf,  3, 0);
    write_be48(buf,  5, ts);
    write_be64(buf, 11, ref);
    return buf;
}

// ---- Write the fixture binary ----
// 7 messages total:
//   [0] StockDirectory  locate=1 "AAPL"
//   [1] StockDirectory  locate=2 "MSFT"
//   [2] AddOrder        locate=1 ref=1 B 200@287.16  ts=34200000000000
//   [3] AddOrder        locate=1 ref=2 B 100@287.15  ts=34200000001000
//   [4] AddOrder        locate=1 ref=3 S 150@287.20  ts=34200000002000
//   [5] AddOrder        locate=1 ref=4 S 300@287.21  ts=34200000003000
//   [6] OrderDelete     locate=1 ref=1               ts=34200000004000
//
// Expected final AAPL book:
//   bids: 287.15 (100 shares, 1 order)   — ref=1 deleted, ref=2 remains
//   asks: 287.20 (150 shares, 1 order)
//         287.21 (300 shares, 1 order)
//   last_update_timestamp: 34200000004000

static const char* FIXTURE_PATH = "/tmp/rest_test_fixture.bin";

static void write_fixture() {
    std::ofstream f(FIXTURE_PATH, std::ios::binary | std::ios::trunc);
    write_frame(f, make_stock_directory(1, "AAPL"));
    write_frame(f, make_stock_directory(2, "MSFT"));
    write_frame(f, make_add_order(1, 1, 'B', 200, 2871600, 34200000000000ULL));
    write_frame(f, make_add_order(1, 2, 'B', 100, 2871500, 34200000001000ULL));
    write_frame(f, make_add_order(1, 3, 'S', 150, 2872000, 34200000002000ULL));
    write_frame(f, make_add_order(1, 4, 'S', 300, 2872100, 34200000003000ULL));
    write_frame(f, make_order_delete(1, 1, 34200000004000ULL));
}

// ---- Test fixture ----

class RestIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        write_fixture();

        publisher_ = std::make_unique<itch::SnapshotPublisher>();
        engine_    = std::make_unique<itch::OrderBookEngine>(*publisher_);
        // Publish after every message so snapshot is ready immediately
        engine_->set_snapshot_interval(1);

        server_ = std::make_unique<itch::RestServer>(*publisher_, 18080);
        server_->start();

        // Run the full pipeline synchronously
        {
            itch::FileFeedReader reader(FIXTURE_PATH);
            itch::MessageParser  parser;
            while (auto buf = reader.next_message()) {
                auto msg = parser.parse(*buf);
                if (msg) engine_->apply(*msg);
            }
        }
        engine_->set_pipeline_complete();

        // Give the REST thread a moment to start accepting connections
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    void TearDown() override {
        server_->stop();
    }

    httplib::Client client_{"localhost", 18080};

    std::unique_ptr<itch::SnapshotPublisher> publisher_;
    std::unique_ptr<itch::OrderBookEngine>   engine_;
    std::unique_ptr<itch::RestServer>        server_;
};

// ---- /status ----

TEST_F(RestIntegrationTest, Status_200_WithCorrectFields) {
    auto res = client_.Get("/status");
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->status, 200);

    // Dump body so we can see exactly what was returned on failure
    SCOPED_TRACE("Response body: " + res->body);

    auto j = json::parse(res->body);
    EXPECT_TRUE(j["pipeline_complete"].get<bool>());
    EXPECT_EQ(j["messages_processed"].get<uint64_t>(), 7u);
    EXPECT_EQ(j["instruments_tracked"].get<size_t>(), 2u);
    EXPECT_GT(j["snapshot_timestamp"].get<uint64_t>(), 0u);
    ASSERT_TRUE(j.contains("snapshot_age_ms"));
    ASSERT_FALSE(j["snapshot_age_ms"].is_null());
    EXPECT_GE(j["snapshot_age_ms"].get<int64_t>(), 0);
}

// ---- /instruments ----

TEST_F(RestIntegrationTest, Instruments_200_BothPresent) {
    auto res = client_.Get("/instruments");
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->status, 200);

    auto j = json::parse(res->body);
    EXPECT_EQ(j["count"].get<size_t>(), 2u);

    bool found_aapl = false, found_msft = false;
    for (const auto& entry : j["instruments"]) {
        std::string sym = entry["symbol"].get<std::string>();
        if (sym == "AAPL") {
            found_aapl = true;
            EXPECT_EQ(entry["trading_state"].get<std::string>(), "T");
            EXPECT_EQ(entry["trading_state_description"].get<std::string>(), "Trading");
            EXPECT_EQ(entry["round_lot_size"].get<uint32_t>(), 100u);
            EXPECT_EQ(entry["market_category"].get<std::string>(), "Q");
        }
        if (sym == "MSFT") found_msft = true;
    }
    EXPECT_TRUE(found_aapl);
    EXPECT_TRUE(found_msft);
}

// ---- /book/AAPL ----

TEST_F(RestIntegrationTest, Book_AAPL_DefaultDepth_CorrectLevels) {
    auto res = client_.Get("/book/AAPL");
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->status, 200);

    auto j = json::parse(res->body);
    EXPECT_EQ(j["symbol"].get<std::string>(), "AAPL");
    EXPECT_EQ(j["bid_count"].get<int>(), 1);
    EXPECT_EQ(j["ask_count"].get<int>(), 2);

    // Best bid: 287.15, 100 shares, 1 order
    EXPECT_NEAR(j["bids"][0]["price"].get<double>(), 287.15, 1e-4);
    EXPECT_EQ(j["bids"][0]["total_shares"].get<uint64_t>(), 100u);
    EXPECT_EQ(j["bids"][0]["order_count"].get<uint32_t>(), 1u);

    // Best ask: 287.20
    EXPECT_NEAR(j["asks"][0]["price"].get<double>(), 287.20, 1e-4);
    EXPECT_EQ(j["asks"][0]["total_shares"].get<uint64_t>(), 150u);

    EXPECT_EQ(j["last_update_timestamp"].get<uint64_t>(), 34200000004000ULL);
}

TEST_F(RestIntegrationTest, Book_AAPL_Depth1_LimitsLevels) {
    auto res = client_.Get("/book/AAPL?depth=1");
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->status, 200);

    auto j = json::parse(res->body);
    EXPECT_EQ(j["bid_count"].get<int>(), 1);
    EXPECT_EQ(j["ask_count"].get<int>(), 1);  // only best ask returned
    EXPECT_EQ(j["asks"].size(), 1u);
}

// ---- /book/AAPL/top ----

TEST_F(RestIntegrationTest, Top_AAPL_CorrectBestBidAskSpread) {
    auto res = client_.Get("/book/AAPL/top");
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->status, 200);

    auto j = json::parse(res->body);
    EXPECT_NEAR(j["best_bid"]["price"].get<double>(), 287.15, 1e-4);
    EXPECT_NEAR(j["best_ask"]["price"].get<double>(), 287.20, 1e-4);
    // Fixed-point: 2872000 - 2871500 = 500 raw = 0.0500
    EXPECT_NEAR(j["spread"].get<double>(), 0.05, 1e-4);
    EXPECT_EQ(j["last_update_timestamp"].get<uint64_t>(), 34200000004000ULL);
}

TEST_F(RestIntegrationTest, Top_CaseInsensitive) {
    auto res = client_.Get("/book/aapl/top");
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->status, 200);

    auto j = json::parse(res->body);
    EXPECT_EQ(j["symbol"].get<std::string>(), "AAPL");
}

// ---- /book/MSFT — empty book ----

TEST_F(RestIntegrationTest, Book_MSFT_EmptyBook) {
    auto res = client_.Get("/book/MSFT");
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->status, 200);

    auto j = json::parse(res->body);
    EXPECT_EQ(j["bid_count"].get<int>(), 0);
    EXPECT_EQ(j["ask_count"].get<int>(), 0);
    EXPECT_TRUE(j["bids"].empty());
    EXPECT_TRUE(j["asks"].empty());
    EXPECT_TRUE(j["last_update_timestamp"].is_null());
}

// ---- Error cases ----

TEST_F(RestIntegrationTest, Book_UnknownSymbol_404) {
    auto res = client_.Get("/book/FAKE");
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->status, 404);

    auto j = json::parse(res->body);
    EXPECT_EQ(j["error"].get<std::string>(), "instrument not found: FAKE");
}

TEST_F(RestIntegrationTest, Book_DepthTooLarge_400) {
    auto res = client_.Get("/book/AAPL?depth=999");
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->status, 400);

    auto j = json::parse(res->body);
    EXPECT_EQ(j["error"].get<std::string>(), "depth must not exceed 50");
}

TEST_F(RestIntegrationTest, Book_DepthZero_400) {
    auto res = client_.Get("/book/AAPL?depth=0");
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->status, 400);

    auto j = json::parse(res->body);
    EXPECT_EQ(j["error"].get<std::string>(), "depth must be at least 1");
}

TEST_F(RestIntegrationTest, Book_DepthNonInteger_400) {
    auto res = client_.Get("/book/AAPL?depth=abc");
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->status, 400);

    auto j = json::parse(res->body);
    EXPECT_EQ(j["error"].get<std::string>(), "depth must be an integer");
}

// ---- Pre-snapshot 503 tests (separate instances, no fixture pipeline) ----

TEST(RestPreSnapshot, Book_Returns503BeforeFirstSnapshot) {
    itch::SnapshotPublisher publisher;
    itch::RestServer server(publisher, 18081);
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    httplib::Client client("localhost", 18081);
    auto res = client.Get("/book/AAPL");
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->status, 503);

    auto j = json::parse(res->body);
    EXPECT_EQ(j["error"].get<std::string>(), "snapshot not yet available");

    server.stop();
}

TEST(RestPreSnapshot, Status_Returns200BeforeFirstSnapshot) {
    itch::SnapshotPublisher publisher;
    itch::RestServer server(publisher, 18082);
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    httplib::Client client("localhost", 18082);
    auto res = client.Get("/status");
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->status, 200);

    auto j = json::parse(res->body);
    EXPECT_EQ(j["messages_processed"].get<uint64_t>(), 0u);
    EXPECT_FALSE(j["pipeline_complete"].get<bool>());

    server.stop();
}