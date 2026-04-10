#include <gtest/gtest.h>
#include <map>
#include <cmath>
#include <functional>
#include "itch/price.h"

using itch::Price;

// Comparison operators

TEST(PriceComparison, EqualValuesCompareEqual) {
    Price a{102500};
    Price b{102500};
    EXPECT_EQ(a, b);
}

TEST(PriceComparison, HigherRawIsGreater) {
    Price lo{102500};
    Price hi{102600};
    EXPECT_LT(lo, hi);
    EXPECT_GT(hi, lo);
}

TEST(PriceComparison, AscendingMapKeyOrder) {
    // Asks side: default ascending — begin() should be lowest price
    std::map<Price, int> asks;
    asks[Price{102600}] = 1;
    asks[Price{102500}] = 2;
    asks[Price{102700}] = 3;
    EXPECT_EQ(asks.begin()->first.raw, 102500u);
}

TEST(PriceComparison, DescendingMapKeyOrder) {
    // Bids side: std::greater — begin() should be highest price
    std::map<Price, int, std::greater<Price>> bids;
    bids[Price{102400}] = 1;
    bids[Price{102600}] = 2;
    bids[Price{102500}] = 3;
    EXPECT_EQ(bids.begin()->first.raw, 102600u);
}

// to_double()

TEST(PriceToDouble, KnownValue) {
    // 102500 raw == $10.2500
    Price p{102500};
    EXPECT_DOUBLE_EQ(p.to_double(), 10.25);
}

TEST(PriceToDouble, Zero) {
    Price p{0};
    EXPECT_DOUBLE_EQ(p.to_double(), 0.0);
}

TEST(PriceToDouble, MaxUint32DoesNotOverflowOrNaN) {
    Price p{UINT32_MAX};
    double v = p.to_double();
    EXPECT_FALSE(std::isnan(v));
    EXPECT_FALSE(std::isinf(v));
    EXPECT_GT(v, 0.0);
}

// Spread fixed-point arithmetic

TEST(PriceSpread, FixedPointSubtractionClean) {
    // 189.43 and 189.42 in fixed-point
    Price ask{1894300};
    Price bid{1894200};

    // Fixed-point subtraction then to_double()
    uint32_t diff_raw = ask.raw - bid.raw;
    double spread_fixed = Price{diff_raw}.to_double();

    // Direct double subtraction — known to produce IEEE 754 noise
    double spread_double = ask.to_double() - bid.to_double();

    // Fixed-point result should be exactly 0.01
    EXPECT_DOUBLE_EQ(spread_fixed, 0.01);

    // Document that double subtraction is not exact
    // (this may pass on some platforms — the point is we don't rely on it)
    (void)spread_double;
}