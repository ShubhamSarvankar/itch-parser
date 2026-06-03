#include "book_engine.h"
#include "snapshot_publisher.h"
#include <iostream>
#include <string>
#include <chrono>
#include <algorithm>
#include <cctype>

namespace itch {

OrderBookEngine::OrderBookEngine(SnapshotPublisher& publisher)
    : publisher_(publisher) {}

// Public interface

void OrderBookEngine::apply(const ParsedMessage& msg) {
    std::visit([this](const auto& m) { this->handle(m); }, msg);
    ++messages_processed_;
    maybe_publish_snapshot();
}

void OrderBookEngine::register_instrument(const InstrumentInfo& info) {
    instruments_[info.stock_locate] = info;
}

const OrderBook* OrderBookEngine::get_book(uint16_t stock_locate) const {
    auto it = books_.find(stock_locate);
    return (it != books_.end()) ? &it->second : nullptr;
}

const OrderRecord* OrderBookEngine::get_order(uint64_t order_ref) const {
    auto it = order_index_.find(order_ref);
    return (it != order_index_.end()) ? &it->second : nullptr;
}

const InstrumentInfo* OrderBookEngine::get_instrument(uint16_t stock_locate) const {
    auto it = instruments_.find(stock_locate);
    return (it != instruments_.end()) ? &it->second : nullptr;
}

// Internal helpers

OrderBook& OrderBookEngine::get_or_create_book(uint16_t stock_locate) {
    auto it = books_.find(stock_locate);
    if (it != books_.end()) return it->second;

    // Unknown locate — create placeholder instrument if needed
    if (instruments_.find(stock_locate) == instruments_.end()) {
        InstrumentInfo info;
        info.stock_locate  = stock_locate;
        info.symbol        = "UNKNOWN_" + std::to_string(stock_locate);
        info.trading_state = 'T';
        instruments_[stock_locate] = info;
        std::cerr << "[WARN] unknown stock_locate " << stock_locate
                  << " — created placeholder\n";
    }

    books_[stock_locate].stock_locate = stock_locate;
    return books_[stock_locate];
}

static inline void refresh_top(itch::OrderBook& book) {
    book.best_bid = book.bids.empty() ? nullptr : &book.bids.begin()->second;
    book.best_ask = book.asks.empty() ? nullptr : &book.asks.begin()->second;
}

// Subtracts sub from val in-place; clamps to zero if sub > val and returns
// true to signal data corruption.
template<typename T>
[[nodiscard]] static bool clamped_sub(T& val, T sub) noexcept {
    if (sub > val) { val = T{0}; return true; }
    val -= sub;
    return false;
}

void OrderBookEngine::remove_shares(OrderBook& book, char side, Price price,
                                    uint32_t shares, bool full_removal) {
    auto remove = [&](auto& side_map) {
        auto it = side_map.find(price);
        if (it == side_map.end()) return;
        PriceLevel& level = it->second;
        // Safety net only: callers pass (before - rec.shares) so this clamp
        // fires only if the level was already desynced by a prior corruption.
        // corrupt_messages_ is not incremented here — it counts bad *input
        // messages* (one per oversized execute/cancel at the handler), not
        // individual subtractions.
        (void)clamped_sub(level.total_shares, static_cast<uint64_t>(shares));
        if (full_removal) --level.order_count;
        if (level.order_count == 0) side_map.erase(it);
    };

    if (side == 'B') remove(book.bids);
    else             remove(book.asks);
    refresh_top(book);  // always resync cached top pointers after any mutation
}

void OrderBookEngine::stamp_book(uint16_t stock_locate, uint64_t timestamp) {
    auto it = books_.find(stock_locate);
    if (it != books_.end()) {
        it->second.last_update_timestamp = timestamp;
    }
}

// Message handlers

void OrderBookEngine::add_order_impl(uint16_t stock_locate, uint64_t order_ref,
                                     char side, uint32_t shares, Price price,
                                     uint64_t timestamp, std::string_view mpid) {
    OrderBook& book = get_or_create_book(stock_locate);

    auto insert = [&](auto& side_map) {
        auto& level        = side_map[price];
        level.price        = price;
        level.total_shares += shares;
        ++level.order_count;
    };

    if (side == 'B') insert(book.bids);
    else             insert(book.asks);

    refresh_top(book);
    OrderRecord rec;
    rec.shares       = shares;
    rec.price        = price;
    rec.stock_locate = stock_locate;
    rec.side         = side;
    if (!mpid.empty()) rec.set_mpid_from(mpid);
    order_index_[order_ref] = rec;

    stamp_book(stock_locate, timestamp);
}

void OrderBookEngine::handle(const AddOrderMsg& m) {
    add_order_impl(m.stock_locate, m.order_ref, m.side, m.shares, m.price,
                   m.timestamp, {});
}

void OrderBookEngine::handle(const AddOrderMPIDMsg& m) {
    add_order_impl(m.stock_locate, m.order_ref, m.side, m.shares, m.price,
                   m.timestamp, m.attribution);
}

void OrderBookEngine::handle(const OrderExecutedMsg& m) {
    auto it = order_index_.find(m.order_ref);
    if (it == order_index_.end()) {
        ++skipped_unknown_ref_;
        return;
    }

    OrderRecord& rec = it->second;
    OrderBook&   book = get_or_create_book(rec.stock_locate);

    uint32_t before = rec.shares;
    if (clamped_sub(rec.shares, m.executed_shares)) ++corrupt_messages_;
    bool full_removal = (rec.shares == 0);

    remove_shares(book, rec.side, rec.price, before - rec.shares, full_removal);
    stamp_book(rec.stock_locate, m.timestamp);

    if (full_removal) {
        order_index_.erase(it);
    }
}

void OrderBookEngine::handle(const OrderExecutedPriceMsg& m) {
    // Book mutation is identical to OrderExecutedMsg —
    // execution_price affects trade reporting, not the resting book
    auto it = order_index_.find(m.order_ref);
    if (it == order_index_.end()) {
        ++skipped_unknown_ref_;
        return;
    }

    OrderRecord& rec = it->second;
    OrderBook&   book = get_or_create_book(rec.stock_locate);

    uint32_t before = rec.shares;
    if (clamped_sub(rec.shares, m.executed_shares)) ++corrupt_messages_;
    bool full_removal = (rec.shares == 0);

    remove_shares(book, rec.side, rec.price, before - rec.shares, full_removal);
    stamp_book(rec.stock_locate, m.timestamp);

    if (full_removal) {
        order_index_.erase(it);
    }
}

void OrderBookEngine::handle(const OrderCancelMsg& m) {
    auto it = order_index_.find(m.order_ref);
    if (it == order_index_.end()) {
        ++skipped_unknown_ref_;
        return;
    }

    OrderRecord& rec = it->second;
    OrderBook&   book = get_or_create_book(rec.stock_locate);

    uint32_t before = rec.shares;
    if (clamped_sub(rec.shares, m.cancelled_shares)) ++corrupt_messages_;
    bool full_removal = (rec.shares == 0);

    remove_shares(book, rec.side, rec.price, before - rec.shares, full_removal);
    stamp_book(rec.stock_locate, m.timestamp);

    if (full_removal) {
        order_index_.erase(it);
    }
}

void OrderBookEngine::handle(const OrderDeleteMsg& m) {
    auto it = order_index_.find(m.order_ref);
    if (it == order_index_.end()) {
        ++skipped_unknown_ref_;
        return;
    }

    OrderRecord& rec = it->second;
    OrderBook&   book = get_or_create_book(rec.stock_locate);

    // Full removal — always decrements order_count and erases level if empty
    remove_shares(book, rec.side, rec.price, rec.shares, true);
    stamp_book(rec.stock_locate, m.timestamp);
    order_index_.erase(it);
}

void OrderBookEngine::handle(const OrderReplaceMsg& m) {
    auto it = order_index_.find(m.original_order_ref);
    if (it == order_index_.end()) {
        ++skipped_unknown_ref_;
        return;
    }

    // Read original record BEFORE erasing — Replace message doesn't carry
    // side, stock_locate, or mpid
    OrderRecord old_rec = it->second;
    order_index_.erase(it);

    OrderBook& book = get_or_create_book(old_rec.stock_locate);

    remove_shares(book, old_rec.side, old_rec.price, old_rec.shares, true);

    // Add replacement order at new price/shares, same side
    auto insert = [&](auto& side_map) {
        auto& level        = side_map[m.price];
        level.price        = m.price;
        level.total_shares += m.shares;
        ++level.order_count;
    };

    if (old_rec.side == 'B') insert(book.bids);
    else                     insert(book.asks);

    refresh_top(book);
    OrderRecord rec;
    rec.shares       = m.shares;
    rec.price        = m.price;
    rec.stock_locate = old_rec.stock_locate;
    rec.side         = old_rec.side;
    rec.mpid         = old_rec.mpid;
    order_index_[m.new_order_ref] = rec;

    stamp_book(old_rec.stock_locate, m.timestamp);
}

void OrderBookEngine::handle(const StockDirectoryMsg& m) {
    InstrumentInfo info;
    info.stock_locate   = m.stock_locate;
    info.symbol         = m.stock;
    info.trading_state  = 'T';  // default until Trading Action says otherwise
    info.round_lot_size = m.round_lot_size;
    info.market_category= m.market_category;
    info.financial_status = m.financial_status;
    instruments_[m.stock_locate] = info;
}

void OrderBookEngine::handle(const StockTradingActionMsg& m) {
    auto it = instruments_.find(m.stock_locate);
    if (it != instruments_.end()) {
        it->second.trading_state = m.trading_state;
    }
}

void OrderBookEngine::log_summary() const {
    if (skipped_unknown_ref_ > 0)
        std::cerr << "[WARN] " << skipped_unknown_ref_
                  << " message(s) skipped: unknown order reference\n";
    if (corrupt_messages_ > 0)
        std::cerr << "[WARN] " << corrupt_messages_
                  << " oversized execute/cancel message(s): rec.shares clamped to zero\n";
}

std::shared_ptr<SystemSnapshot> OrderBookEngine::build_snapshot_for_test() const {
    return build_snapshot();
}

void OrderBookEngine::set_pipeline_complete() {
    pipeline_complete_ = true;
    // Publish a final snapshot so the REST layer sees the last state
    publisher_.publish(build_snapshot());
}

void OrderBookEngine::maybe_publish_snapshot() {
    if (messages_processed_ % snapshot_interval_ == 0) {
        publisher_.publish(build_snapshot());
    }
}

std::shared_ptr<SystemSnapshot> OrderBookEngine::build_snapshot() const {
    auto snap = std::make_shared<SystemSnapshot>();
    snap->messages_processed = messages_processed_;
    snap->pipeline_complete  = pipeline_complete_;

    // Wall clock timestamp in ms since epoch
    snap->snapshot_timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );

    for (const auto& [locate, book] : books_) {
        auto inst_it = instruments_.find(locate);
        if (inst_it == instruments_.end()) continue;

        const InstrumentInfo& info = inst_it->second;
        OrderBookSnapshot book_snap;
        book_snap.symbol        = info.symbol;
        book_snap.trading_state = info.trading_state;
        book_snap.last_update_timestamp = book.last_update_timestamp;
        book_snap.stock_locate  = info.stock_locate;
        book_snap.round_lot_size= info.round_lot_size;
        book_snap.market_category = info.market_category;

        book_snap.bids.reserve(book.bids.size());
        book_snap.asks.reserve(book.asks.size());

        for (const auto& [price, level] : book.bids) {
            book_snap.bids.push_back({
                price.to_double(),
                level.total_shares,
                level.order_count
            });
            // bids map iterates highest-first (std::greater) — correct order
        }

        for (const auto& [price, level] : book.asks) {
            book_snap.asks.push_back({
                price.to_double(),
                level.total_shares,
                level.order_count
            });
            // asks map iterates lowest-first (default) — correct order
        }

        snap->books[info.symbol] = std::move(book_snap);
    }

    // Include instruments that have no book activity yet
    for (const auto& [locate, info] : instruments_) {
        if (snap->books.find(info.symbol) == snap->books.end()) {
            OrderBookSnapshot empty_snap;
            empty_snap.symbol        = info.symbol;
            empty_snap.trading_state = info.trading_state;
            empty_snap.last_update_timestamp = 0;
            empty_snap.stock_locate  = info.stock_locate;
            empty_snap.round_lot_size= info.round_lot_size;
            empty_snap.market_category = info.market_category;
            snap->books[info.symbol] = std::move(empty_snap);
        }
    }

    // Build lowercase → canonical index once so REST handlers do O(1) lookup
    snap->symbol_index.reserve(snap->books.size());
    for (const auto& [sym, _] : snap->books) {
        std::string lo = sym;
        std::transform(lo.begin(), lo.end(), lo.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        snap->symbol_index[lo] = sym;
    }

    return snap;
}

} // namespace itch