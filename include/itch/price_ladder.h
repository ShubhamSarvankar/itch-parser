// include/itch/price_ladder.h
//
// Three price-ladder implementations behind a common minimal interface.
//
// All three support the same operations the OrderBookEngine needs:
//   add_shares(price, shares)       merge into level (creating if absent)
//   remove_shares(price, shares,
//                 full_removal)     undo of add_shares, may erase the level
//   best()                          pointer to best level, nullptr if empty
//   size()                          number of distinct price levels
//   for_each(fn)                    visit levels in best-first order
//
// "Best-first" depends on side:
//   bid:  descending price
//   ask:  ascending price
//
// The Side template parameter encodes that direction so the binary-search
// and the array iteration both produce best-first without runtime branches.

#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>
#include <utility>
#include "order_book.h"
#include "price.h"

namespace itch {

enum class Side : char { Bid = 'B', Ask = 'S' };

namespace detail {
template <Side S> struct CompareBest;

// Best bid is highest price. std::greater orders the map so begin() is the
// highest.
template <> struct CompareBest<Side::Bid> { using map_compare = std::greater<Price>; };
// Best ask is lowest price.
template <> struct CompareBest<Side::Ask> { using map_compare = std::less<Price>; };
} // namespace detail

// Map-backed ladder. Current behavior. Baseline.
template <Side S>
class MapLadder {
public:
    using map_t = std::map<Price, PriceLevel,
                           typename detail::CompareBest<S>::map_compare>;

    void add_shares(Price p, uint32_t shares) {
        auto& lvl = levels_[p];
        lvl.price = p;
        lvl.total_shares += shares;
        ++lvl.order_count;
    }

    void remove_shares(Price p, uint32_t shares, bool full_removal) {
        auto it = levels_.find(p);
        if (it == levels_.end()) return;
        PriceLevel& lvl = it->second;
        lvl.total_shares -= shares;
        if (full_removal) --lvl.order_count;
        if (lvl.order_count == 0) levels_.erase(it);
    }

    const PriceLevel* best() const {
        return levels_.empty() ? nullptr : &levels_.begin()->second;
    }

    std::size_t size() const { return levels_.size(); }

    template <class Fn>
    void for_each(Fn&& fn) const {
        for (const auto& [p, lvl] : levels_) fn(lvl);
    }

private:
    map_t levels_;
};

// Sorted-vector ladder. Stores PriceLevel sorted in best-first order.
// add_shares does a binary search; on miss it shifts the tail down to
// insert. Removal shifts the tail up. Good when level count is small
// (under a few hundred) and especially good for full iteration.
template <Side S>
class FlatLadder {
public:
    // True when a < b means a is "more in front" (closer to best) for this side.
    static bool better(Price a, Price b) {
        if constexpr (S == Side::Bid) return a.raw > b.raw;
        else                          return a.raw < b.raw;
    }

    void add_shares(Price p, uint32_t shares) {
        // Lower bound: first element that is NOT better than p, i.e. the
        // first equal or worse. If it equals p we merge; otherwise we
        // insert before it.
        auto it = std::lower_bound(
            levels_.begin(), levels_.end(), p,
            [](const PriceLevel& lvl, Price q) { return better(lvl.price, q); });
        if (it != levels_.end() && it->price == p) {
            it->total_shares += shares;
            ++it->order_count;
            return;
        }
        PriceLevel nl;
        nl.price        = p;
        nl.total_shares = shares;
        nl.order_count  = 1;
        levels_.insert(it, nl);
    }

    void remove_shares(Price p, uint32_t shares, bool full_removal) {
        auto it = std::lower_bound(
            levels_.begin(), levels_.end(), p,
            [](const PriceLevel& lvl, Price q) { return better(lvl.price, q); });
        if (it == levels_.end() || it->price != p) return;
        it->total_shares -= shares;
        if (full_removal) --it->order_count;
        if (it->order_count == 0) levels_.erase(it);
    }

    const PriceLevel* best() const {
        return levels_.empty() ? nullptr : &levels_.front();
    }

    std::size_t size() const { return levels_.size(); }

    template <class Fn>
    void for_each(Fn&& fn) const {
        for (const auto& lvl : levels_) fn(lvl);
    }

private:
    std::vector<PriceLevel> levels_;
};

// Array ladder. Fixed-size dense array of PriceLevel indexed by
// (price - base) / tick. Best is tracked by an index that walks up/down
// after every mutation.
//
// Trade off: needs a price band fixed at construction. For NASDAQ
// equities above $1.00, tick is 100 (in raw units), and a price band of
// 1024 slots covers a $10.24 window per book around its trading range.
// In production, slots would be sized per instrument or carved out in
// per-instrument arenas.
template <Side S, uint32_t SLOTS = 1024, uint32_t TICK = 100>
class ArrayLadder {
public:
    explicit ArrayLadder(uint32_t base_raw = 0) : base_raw_(base_raw) {
        levels_.fill(PriceLevel{});
        nonempty_count_ = 0;
        best_idx_ = SLOTS; // sentinel = empty
    }

    void set_base(uint32_t base_raw) { base_raw_ = base_raw; }

    void add_shares(Price p, uint32_t shares) {
        uint32_t i = idx_of(p);
        if (i >= SLOTS) return; // out of band; caller should size correctly
        PriceLevel& lvl = levels_[i];
        if (lvl.order_count == 0) {
            ++nonempty_count_;
            lvl.price = p;
            update_best_after_add(i);
        }
        lvl.total_shares += shares;
        ++lvl.order_count;
    }

    void remove_shares(Price p, uint32_t shares, bool full_removal) {
        uint32_t i = idx_of(p);
        if (i >= SLOTS) return;
        PriceLevel& lvl = levels_[i];
        if (lvl.order_count == 0) return;
        lvl.total_shares -= shares;
        if (full_removal) --lvl.order_count;
        if (lvl.order_count == 0) {
            --nonempty_count_;
            if (i == best_idx_) advance_best_from(i);
        }
    }

    const PriceLevel* best() const {
        return nonempty_count_ == 0 ? nullptr : &levels_[best_idx_];
    }

    std::size_t size() const { return nonempty_count_; }

    template <class Fn>
    void for_each(Fn&& fn) const {
        if (nonempty_count_ == 0) return;
        if constexpr (S == Side::Bid) {
            // Bids: best is highest price => highest index in storage
            // (because idx_of grows with price). Iterate downward.
            for (uint32_t i = best_idx_ + 1; i-- > 0;) {
                if (levels_[i].order_count > 0) fn(levels_[i]);
            }
        } else {
            for (uint32_t i = best_idx_; i < SLOTS; ++i) {
                if (levels_[i].order_count > 0) fn(levels_[i]);
            }
        }
    }

private:
    inline uint32_t idx_of(Price p) const {
        if (p.raw < base_raw_) return SLOTS;
        uint32_t off = (p.raw - base_raw_) / TICK;
        return off; // SLOTS sentinel handled by callers
    }

    void update_best_after_add(uint32_t i) {
        if (nonempty_count_ == 1) { best_idx_ = i; return; }
        if constexpr (S == Side::Bid) {
            if (i > best_idx_) best_idx_ = i; // higher price is better
        } else {
            if (i < best_idx_) best_idx_ = i; // lower price is better
        }
    }

    void advance_best_from(uint32_t i) {
        if (nonempty_count_ == 0) { best_idx_ = SLOTS; return; }
        if constexpr (S == Side::Bid) {
            while (i > 0) {
                --i;
                if (levels_[i].order_count > 0) { best_idx_ = i; return; }
            }
        } else {
            while (i + 1 < SLOTS) {
                ++i;
                if (levels_[i].order_count > 0) { best_idx_ = i; return; }
            }
        }
        best_idx_ = SLOTS; // empty
    }

    uint32_t base_raw_{0};
    uint32_t best_idx_{SLOTS};
    uint32_t nonempty_count_{0};
    std::array<PriceLevel, SLOTS> levels_{};
};

} // namespace itch
