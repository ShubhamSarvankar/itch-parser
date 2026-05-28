// include/itch/order_record.h
//
// Slim version of OrderRecord. Goal: fit two records per 64 byte cache line.
//
// Layout, 16 bytes total:
//   uint32_t shares           4
//   uint32_t price_raw        4   (Price stores a single uint32_t internally)
//   uint16_t stock_locate     2
//   char     side             1
//   char     mpid[4]          4   (NASDAQ MPID is 4 chars, space padded)
//   char     _pad             1
//
// Removed fields:
//   - order_ref: this is the key into order_index_, redundant in the value.
//   - std::string mpid: heap allocation on every Add. Fixed 4 char array.
//
// All handlers in book_engine.cpp read side / price / shares / stock_locate
// (and only OrderReplace reads mpid). No handler reads order_ref off the
// record value.

#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>
#include "price.h"

namespace itch {

struct OrderRecord {
    uint32_t            shares{};
    Price               price{};
    uint16_t            stock_locate{};
    char                side{};
    std::array<char, 4> mpid{};

    std::string_view mpid_view() const {
        // Strip trailing spaces / NULs for display, leave raw bytes for storage.
        std::size_t n = mpid.size();
        while (n > 0 && (mpid[n - 1] == ' ' || mpid[n - 1] == '\0')) --n;
        return std::string_view(mpid.data(), n);
    }

    void set_mpid_from(std::string_view s) {
        mpid.fill(' ');
        std::size_t n = std::min<std::size_t>(s.size(), 4);
        std::memcpy(mpid.data(), s.data(), n);
    }
};

static_assert(sizeof(OrderRecord) <= 16,
              "OrderRecord must fit in 16 bytes for cache density");

} // namespace itch
