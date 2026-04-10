#include <gtest/gtest.h>
#include "book_engine.h"
#include "snapshot_publisher.h"

// Every test gets a fresh engine via this fixture
class BookEngineTest : public ::testing::Test {
protected:
    itch::SnapshotPublisher publisher;
    itch::OrderBookEngine   engine{publisher};

    // Seed one instrument so tests don't have to send a StockDirectoryMsg
    void SetUp() override {
        itch::InstrumentInfo info;
        info.stock_locate   = 1;
        info.symbol         = "AAPL";
        info.trading_state  = 'T';
        info.round_lot_size = 100;
        engine.register_instrument(info);
    }

    // Helpers to build messages without touching the parser
    itch::AddOrderMsg add(uint64_t ref, char side, uint32_t shares,
                          uint32_t price_raw, uint16_t locate = 1) {
        itch::AddOrderMsg m;
        m.stock_locate = locate;
        m.order_ref    = ref;
        m.side         = side;
        m.shares       = shares;
        m.price.raw    = price_raw;
        return m;
    }

    itch::OrderExecutedMsg execute(uint64_t ref, uint32_t shares) {
        itch::OrderExecutedMsg m;
        m.order_ref       = ref;
        m.executed_shares = shares;
        return m;
    }

    itch::OrderCancelMsg cancel(uint64_t ref, uint32_t shares) {
        itch::OrderCancelMsg m;
        m.order_ref        = ref;
        m.cancelled_shares = shares;
        return m;
    }

    itch::OrderDeleteMsg del(uint64_t ref) {
        itch::OrderDeleteMsg m;
        m.order_ref = ref;
        return m;
    }

    itch::OrderReplaceMsg replace(uint64_t orig, uint64_t next,
                                  uint32_t shares, uint32_t price_raw) {
        itch::OrderReplaceMsg m;
        m.original_order_ref = orig;
        m.new_order_ref      = next;
        m.shares             = shares;
        m.price.raw          = price_raw;
        return m;
    }
};

// Add Order

TEST_F(BookEngineTest, AddOrder_BidCreatesLevel) {
    engine.apply(add(1, 'B', 500, 1000000));
    const auto* book = engine.get_book(1);
    ASSERT_NE(book, nullptr);
    ASSERT_EQ(book->bids.size(), 1u);
    const auto& level = book->bids.begin()->second;
    EXPECT_EQ(level.total_shares, 500u);
    EXPECT_EQ(level.order_count,  1u);
    EXPECT_EQ(level.price.raw,    1000000u);
}

TEST_F(BookEngineTest, AddOrder_AskCreatesLevel) {
    engine.apply(add(1, 'S', 300, 1000100));
    const auto* book = engine.get_book(1);
    ASSERT_NE(book, nullptr);
    ASSERT_EQ(book->asks.size(), 1u);
    const auto& level = book->asks.begin()->second;
    EXPECT_EQ(level.total_shares, 300u);
    EXPECT_EQ(level.order_count,  1u);
}

TEST_F(BookEngineTest, AddOrder_TwoOrdersSameLevel) {
    engine.apply(add(1, 'B', 200, 1000000));
    engine.apply(add(2, 'B', 150, 1000000));
    const auto* book = engine.get_book(1);
    ASSERT_EQ(book->bids.size(), 1u);
    const auto& level = book->bids.begin()->second;
    EXPECT_EQ(level.total_shares, 350u);
    EXPECT_EQ(level.order_count,  2u);
}

TEST_F(BookEngineTest, AddOrder_TwoOrdersDifferentLevels) {
    engine.apply(add(1, 'B', 200, 1000000));
    engine.apply(add(2, 'B', 100, 999900));
    const auto* book = engine.get_book(1);
    EXPECT_EQ(book->bids.size(), 2u);
}

TEST_F(BookEngineTest, AddOrder_BidBestPriceFirst) {
    engine.apply(add(1, 'B', 100, 999900));
    engine.apply(add(2, 'B', 100, 1000000));
    const auto* book = engine.get_book(1);
    // begin() on bids (std::greater) should be highest price
    EXPECT_EQ(book->bids.begin()->first.raw, 1000000u);
}

TEST_F(BookEngineTest, AddOrder_AskBestPriceFirst) {
    engine.apply(add(1, 'S', 100, 1000100));
    engine.apply(add(2, 'S', 100, 1000000));
    const auto* book = engine.get_book(1);
    // begin() on asks (ascending) should be lowest price
    EXPECT_EQ(book->asks.begin()->first.raw, 1000000u);
}

TEST_F(BookEngineTest, AddOrderMPID_SameBookStateAsAddOrder) {
    itch::AddOrderMPIDMsg m;
    m.stock_locate = 1;
    m.order_ref    = 1;
    m.side         = 'B';
    m.shares       = 400;
    m.price.raw    = 1000000;
    m.attribution  = "NSDQ";
    engine.apply(m);

    const auto* book = engine.get_book(1);
    ASSERT_NE(book, nullptr);
    EXPECT_EQ(book->bids.begin()->second.total_shares, 400u);
    EXPECT_EQ(book->bids.begin()->second.order_count,  1u);

    const auto* rec = engine.get_order(1);
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(rec->mpid, "NSDQ");
}

// Order Cancel

TEST_F(BookEngineTest, Cancel_PartialReducesShares) {
    engine.apply(add(1, 'B', 500, 1000000));
    engine.apply(cancel(1, 200));

    const auto* book = engine.get_book(1);
    EXPECT_EQ(book->bids.begin()->second.total_shares, 300u);
    EXPECT_EQ(book->bids.begin()->second.order_count,  1u);  // still resting

    const auto* rec = engine.get_order(1);
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(rec->shares, 300u);
}

TEST_F(BookEngineTest, Cancel_FullRemovesLevel) {
    engine.apply(add(1, 'B', 500, 1000000));
    engine.apply(cancel(1, 500));

    const auto* book = engine.get_book(1);
    EXPECT_TRUE(book->bids.empty());
    EXPECT_EQ(engine.get_order(1), nullptr);
}

TEST_F(BookEngineTest, Cancel_FullLeavesOtherOrderAtLevel) {
    engine.apply(add(1, 'B', 200, 1000000));
    engine.apply(add(2, 'B', 100, 1000000));
    engine.apply(cancel(1, 200));  // fully cancel order 1

    const auto* book = engine.get_book(1);
    ASSERT_EQ(book->bids.size(), 1u);
    EXPECT_EQ(book->bids.begin()->second.total_shares, 100u);
    EXPECT_EQ(book->bids.begin()->second.order_count,  1u);
}

// Order Execute

TEST_F(BookEngineTest, Execute_PartialReducesShares) {
    engine.apply(add(1, 'S', 300, 1000100));
    engine.apply(execute(1, 100));

    const auto* book = engine.get_book(1);
    EXPECT_EQ(book->asks.begin()->second.total_shares, 200u);
    EXPECT_EQ(book->asks.begin()->second.order_count,  1u);

    const auto* rec = engine.get_order(1);
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(rec->shares, 200u);
}

TEST_F(BookEngineTest, Execute_FullRemovesLevel) {
    engine.apply(add(1, 'S', 100, 1000100));
    engine.apply(execute(1, 100));

    const auto* book = engine.get_book(1);
    EXPECT_TRUE(book->asks.empty());
    EXPECT_EQ(engine.get_order(1), nullptr);
}

TEST_F(BookEngineTest, ExecuteWithPrice_SameBookMutationAsExecute) {
    engine.apply(add(1, 'B', 200, 1000000));

    itch::OrderExecutedPriceMsg m;
    m.order_ref         = 1;
    m.executed_shares   = 100;
    m.printable         = 'Y';
    m.execution_price.raw = 999999;  // different from order price — doesn't affect book
    engine.apply(m);

    const auto* book = engine.get_book(1);
    EXPECT_EQ(book->bids.begin()->second.total_shares, 100u);
    EXPECT_EQ(book->bids.begin()->second.order_count,  1u);
}

// Order Delete

TEST_F(BookEngineTest, Delete_RemovesOrderAndLevel) {
    engine.apply(add(1, 'B', 300, 1000000));
    engine.apply(del(1));

    const auto* book = engine.get_book(1);
    EXPECT_TRUE(book->bids.empty());
    EXPECT_EQ(engine.get_order(1), nullptr);
}

TEST_F(BookEngineTest, Delete_LeavesOtherOrderAtLevel) {
    engine.apply(add(1, 'B', 200, 1000000));
    engine.apply(add(2, 'B', 150, 1000000));
    engine.apply(del(1));

    const auto* book = engine.get_book(1);
    ASSERT_EQ(book->bids.size(), 1u);
    EXPECT_EQ(book->bids.begin()->second.total_shares, 150u);
    EXPECT_EQ(book->bids.begin()->second.order_count,  1u);
    EXPECT_EQ(engine.get_order(1), nullptr);
    EXPECT_NE(engine.get_order(2), nullptr);
}

// Order Replace

TEST_F(BookEngineTest, Replace_MovesToNewPriceLevel) {
    engine.apply(add(1, 'S', 400, 501000));
    engine.apply(replace(1, 2, 350, 501500));

    const auto* book = engine.get_book(1);
    EXPECT_TRUE(book->asks.find(itch::Price{501000}) == book->asks.end());

    auto it = book->asks.find(itch::Price{501500});
    ASSERT_NE(it, book->asks.end());
    EXPECT_EQ(it->second.total_shares, 350u);
    EXPECT_EQ(it->second.order_count,  1u);

    EXPECT_EQ(engine.get_order(1), nullptr);
    const auto* rec = engine.get_order(2);
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(rec->price.raw, 501500u);
    EXPECT_EQ(rec->shares,    350u);
    EXPECT_EQ(rec->side,      'S');  // side preserved from original
}

TEST_F(BookEngineTest, Replace_PreservesSide) {
    engine.apply(add(1, 'B', 100, 1000000));
    engine.apply(replace(1, 2, 100, 1000100));

    const auto* rec = engine.get_order(2);
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(rec->side, 'B');
}

// Error / edge cases

TEST_F(BookEngineTest, UnknownRefExecute_Skipped) {
    engine.apply(execute(9999, 100));
    EXPECT_EQ(engine.skipped_unknown_ref(), 1u);
}

TEST_F(BookEngineTest, UnknownRefCancel_Skipped) {
    engine.apply(cancel(9999, 100));
    EXPECT_EQ(engine.skipped_unknown_ref(), 1u);
}

TEST_F(BookEngineTest, UnknownRefDelete_Skipped) {
    engine.apply(del(9999));
    EXPECT_EQ(engine.skipped_unknown_ref(), 1u);
}

TEST_F(BookEngineTest, UnknownRefReplace_Skipped) {
    engine.apply(replace(9999, 10000, 100, 1000000));
    EXPECT_EQ(engine.skipped_unknown_ref(), 1u);
}

TEST_F(BookEngineTest, UnknownRef_BookUnchanged) {
    engine.apply(add(1, 'B', 100, 1000000));
    engine.apply(execute(9999, 50));  // unknown ref — should not touch book
    const auto* book = engine.get_book(1);
    EXPECT_EQ(book->bids.begin()->second.total_shares, 100u);
}

// Integration scenarios

TEST_F(BookEngineTest, Scenario1_PartialFillThenCancel) {
    engine.apply(add(1, 'B', 500, 1000000));
    engine.apply(add(2, 'B', 300, 999500));
    engine.apply(execute(1, 200));
    engine.apply(cancel(1, 100));

    const auto* book = engine.get_book(1);

    // Level 100.00: 200 shares remaining
    auto it = book->bids.find(itch::Price{1000000});
    ASSERT_NE(it, book->bids.end());
    EXPECT_EQ(it->second.total_shares, 200u);
    EXPECT_EQ(it->second.order_count,  1u);

    // Level 99.50: untouched
    auto it2 = book->bids.find(itch::Price{999500});
    ASSERT_NE(it2, book->bids.end());
    EXPECT_EQ(it2->second.total_shares, 300u);

    // Both orders still in index
    const auto* rec1 = engine.get_order(1);
    ASSERT_NE(rec1, nullptr);
    EXPECT_EQ(rec1->shares, 200u);
    EXPECT_NE(engine.get_order(2), nullptr);
}

TEST_F(BookEngineTest, Scenario2_ReplaceMovesPriceLevel) {
    engine.apply(add(1, 'S', 400, 501000));
    engine.apply(replace(1, 2, 350, 501500));

    const auto* book = engine.get_book(1);
    EXPECT_TRUE(book->asks.find(itch::Price{501000}) == book->asks.end());

    auto it = book->asks.find(itch::Price{501500});
    ASSERT_NE(it, book->asks.end());
    EXPECT_EQ(it->second.total_shares, 350u);
    EXPECT_EQ(it->second.order_count,  1u);

    EXPECT_EQ(engine.get_order(1), nullptr);
    const auto* rec = engine.get_order(2);
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(rec->price.raw, 501500u);
    EXPECT_EQ(rec->shares,    350u);
}

TEST_F(BookEngineTest, Scenario3_FullExecutionClearsLevel) {
    engine.apply(add(1, 'S', 100, 750000));
    engine.apply(execute(1, 100));

    const auto* book = engine.get_book(1);
    EXPECT_TRUE(book->asks.empty());
    EXPECT_EQ(engine.get_order(1), nullptr);
}

TEST_F(BookEngineTest, Scenario4_MultipleOrdersAtLevel_PartialRemoval) {
    engine.apply(add(1, 'B', 200, 300000));
    engine.apply(add(2, 'B', 150, 300000));
    engine.apply(del(1));

    const auto* book = engine.get_book(1);
    auto it = book->bids.find(itch::Price{300000});
    ASSERT_NE(it, book->bids.end());
    EXPECT_EQ(it->second.total_shares, 150u);
    EXPECT_EQ(it->second.order_count,  1u);

    EXPECT_EQ(engine.get_order(1), nullptr);
    EXPECT_NE(engine.get_order(2), nullptr);
}

TEST_F(BookEngineTest, Scenario5_MidSessionStart_UnknownRefs) {
    engine.apply(execute(9999, 50));   // never seen
    engine.apply(cancel(8888, 25));    // never seen
    engine.apply(add(1, 'B', 100, 200000));

    EXPECT_EQ(engine.skipped_unknown_ref(), 2u);

    const auto* book = engine.get_book(1);
    auto it = book->bids.find(itch::Price{200000});
    ASSERT_NE(it, book->bids.end());
    EXPECT_EQ(it->second.total_shares, 100u);

    EXPECT_NE(engine.get_order(1), nullptr);
}

// Milestone 6 — Instrument registry and error handling

TEST_F(BookEngineTest, StockDirectory_PopulatesRegistry) {
    itch::StockDirectoryMsg m;
    m.stock_locate    = 99;
    m.stock           = "TSLA";
    m.market_category = 'Q';
    m.financial_status= 'N';
    m.round_lot_size  = 100;
    engine.apply(m);

    const auto* info = engine.get_instrument(99);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->symbol,         "TSLA");
    EXPECT_EQ(info->stock_locate,   99);
    EXPECT_EQ(info->round_lot_size, 100u);
    EXPECT_EQ(info->market_category,'Q');
    // Default trading state on directory message is 'T'
    EXPECT_EQ(info->trading_state,  'T');
}

TEST_F(BookEngineTest, TradingAction_UpdatesState) {
    // Instrument 1 ("AAPL") seeded in SetUp as 'T'
    itch::StockTradingActionMsg m;
    m.stock_locate  = 1;
    m.trading_state = 'H';
    engine.apply(m);

    const auto* info = engine.get_instrument(1);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->trading_state, 'H');
}

TEST_F(BookEngineTest, TradingAction_RestoresAfterHalt) {
    itch::StockTradingActionMsg halt;
    halt.stock_locate  = 1;
    halt.trading_state = 'H';
    engine.apply(halt);

    itch::StockTradingActionMsg resume;
    resume.stock_locate  = 1;
    resume.trading_state = 'T';
    engine.apply(resume);

    EXPECT_EQ(engine.get_instrument(1)->trading_state, 'T');
}

TEST_F(BookEngineTest, TradingAction_AllFourStates) {
    for (char state : {'T', 'H', 'P', 'Q'}) {
        itch::StockTradingActionMsg m;
        m.stock_locate  = 1;
        m.trading_state = state;
        engine.apply(m);
        EXPECT_EQ(engine.get_instrument(1)->trading_state, state);
    }
}

TEST_F(BookEngineTest, UnknownLocate_CreatesPlaceholder) {
    // Send an Add Order for a locate code with no Stock Directory message
    engine.apply(add(1, 'B', 100, 1000000, /*locate=*/77));

    const auto* info = engine.get_instrument(77);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->symbol, "UNKNOWN_77");

    // Book should still be updated
    const auto* book = engine.get_book(77);
    ASSERT_NE(book, nullptr);
    EXPECT_EQ(book->bids.begin()->second.total_shares, 100u);
}

TEST_F(BookEngineTest, UnknownLocate_SymbolResolvesAfterDirectory) {
    // First, an order arrives for unknown locate 55
    engine.apply(add(1, 'B', 100, 1000000, /*locate=*/55));
    EXPECT_EQ(engine.get_instrument(55)->symbol, "UNKNOWN_55");

    // Then a Stock Directory message arrives for the same locate
    itch::StockDirectoryMsg dir;
    dir.stock_locate    = 55;
    dir.stock           = "META";
    dir.round_lot_size  = 100;
    dir.market_category = 'Q';
    dir.financial_status= 'N';
    engine.apply(dir);

    // Instrument should now be resolved
    EXPECT_EQ(engine.get_instrument(55)->symbol, "META");

    // Book for locate 55 still intact
    const auto* book = engine.get_book(55);
    ASSERT_NE(book, nullptr);
    EXPECT_EQ(book->bids.begin()->second.total_shares, 100u);
}

TEST_F(BookEngineTest, MessagesProcessed_CountsAllTypes) {
    engine.apply(add(1, 'B', 100, 1000000));
    engine.apply(execute(1, 50));
    engine.apply(cancel(1, 25));

    EXPECT_EQ(engine.messages_processed(), 3u);
}

TEST_F(BookEngineTest, SkippedUnknownRef_MultipleOpsAccumulate) {
    engine.apply(execute(9001, 100));
    engine.apply(cancel(9002, 50));
    engine.apply(del(9003));
    engine.apply(replace(9004, 9005, 100, 1000000));

    EXPECT_EQ(engine.skipped_unknown_ref(), 4u);
}