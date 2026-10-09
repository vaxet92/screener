#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "md_core/indicators/ema.h"
#include "md_core/indicators/int_math.h"
#include "md_core/indicators/turnover_window.h"
#include "md_core/indicators/wilder_atr.h"

using namespace screener;

namespace {

constexpr Price P(double real) {
    return static_cast<Price>(real * static_cast<double>(kPriceScale));
}
constexpr Volume V(double real) {
    return static_cast<Volume>(real * static_cast<double>(kVolumeScale));
}

// Floating-point reference, used ONLY as a test oracle.
//
// The integer indicators are deliberately NOT bit-identical to this: rounding
// division leaves a bounded error (DESIGN.md §3), so these tests assert a
// TOLERANCE rather than equality. A test demanding 1e-9 here would be
// asserting that the implementation is a double, which is exactly what it is
// not allowed to be.
double EmaReference(const std::vector<double>& closes, int period) {
    double sum = 0.0;
    for (int i = 0; i < period; ++i) {
        sum += closes[static_cast<std::size_t>(i)];
    }
    double ema = sum / period;
    const double alpha = 2.0 / (period + 1);
    for (std::size_t i = static_cast<std::size_t>(period); i < closes.size(); ++i) {
        ema += (closes[i] - ema) * alpha;
    }
    return ema;
}

}  // namespace

// ---------------------------------------------------------------------------
// RoundedDiv
// ---------------------------------------------------------------------------

TEST(IntMath, RoundsHalfAwayFromZero) {
    EXPECT_EQ(RoundedDiv(10, 4), 3);    // 2.5 -> 3
    EXPECT_EQ(RoundedDiv(-10, 4), -3);  // -2.5 -> -3, symmetric
    EXPECT_EQ(RoundedDiv(9, 4), 2);     // 2.25 -> 2
    EXPECT_EQ(RoundedDiv(-9, 4), -2);
    EXPECT_EQ(RoundedDiv(0, 7), 0);
}

// Truncation would bias every step in the same direction, and a recursive
// filter turns a constant bias into a drift. This is the property that makes
// rounding mandatory rather than cosmetic.
TEST(IntMath, SymmetricSoAConstantSignalDoesNotDrift) {
    int64_t value = 1000;
    for (int i = 0; i < 10'000; ++i) {
        const int64_t delta = 1000 - value;
        value += RoundedDiv(delta * 2, 51);
    }
    EXPECT_EQ(value, 1000);
}

// ---------------------------------------------------------------------------
// Ema
// ---------------------------------------------------------------------------

TEST(Ema, NotReadyBeforePeriodBars) {
    Ema<50> ema;
    for (int i = 0; i < 49; ++i) {
        ema.Update(P(100.0));
        EXPECT_FALSE(ema.Ready()) << "ready after " << (i + 1) << " bars";
    }
    ema.Update(P(100.0));
    EXPECT_TRUE(ema.Ready());
}

TEST(Ema, SeedsWithSimpleMeanOfFirstPeriod) {
    Ema<4> ema;
    ema.Update(P(10.0));
    ema.Update(P(20.0));
    ema.Update(P(30.0));
    ema.Update(P(40.0));
    ASSERT_TRUE(ema.Ready());
    EXPECT_EQ(ema.Value(), P(25.0));  // (10+20+30+40)/4
}

TEST(Ema, ConstantSeriesStaysAtThatConstant) {
    Ema<50> ema;
    for (int i = 0; i < 500; ++i) {
        ema.Update(P(1234.5));
    }
    EXPECT_EQ(ema.Value(), P(1234.5));
}

TEST(Ema, MatchesFloatingReferenceWithinTolerance) {
    std::vector<double> closes;
    for (int i = 0; i < 600; ++i) {
        closes.push_back(100.0 + 10.0 * std::sin(i * 0.1) + 0.01 * i);
    }

    Ema<50> ema;
    for (double close : closes) {
        ema.Update(P(close));
    }
    ASSERT_TRUE(ema.Ready());

    const double expected = EmaReference(closes, 50);
    const double actual = static_cast<double>(ema.Value()) / static_cast<double>(kPriceScale);

    // Relative tolerance, not absolute: the error budget in DESIGN.md §3 is
    // ~13 units at kPriceScale per step, and 550 recursive steps keep it well
    // inside 1e-6 relative.
    EXPECT_NEAR(actual, expected, std::abs(expected) * 1e-6);
}

// The cheapest contracts are the ones a coarse scale would destroy. At
// kPriceScale = 1e10 a price of 0.000006 still has four significant digits.
TEST(Ema, WorksAtTheMicroPriceExtreme) {
    Ema<50> ema;
    for (int i = 0; i < 200; ++i) {
        ema.Update(P(0.000006));
    }
    ASSERT_TRUE(ema.Ready());
    EXPECT_EQ(ema.Value(), P(0.000006));
}

TEST(Ema, WorksAtBtcScale) {
    Ema<50> ema;
    for (int i = 0; i < 200; ++i) {
        ema.Update(P(95'000.0));
    }
    ASSERT_TRUE(ema.Ready());
    EXPECT_EQ(ema.Value(), P(95'000.0));
}

TEST(Ema, ResetClearsReadiness) {
    Ema<4> ema;
    for (int i = 0; i < 10; ++i) {
        ema.Update(P(10.0));
    }
    ASSERT_TRUE(ema.Ready());
    ema.Reset();
    EXPECT_FALSE(ema.Ready());
    EXPECT_EQ(ema.Bars(), 0u);
}

// ---------------------------------------------------------------------------
// WilderAtr
// ---------------------------------------------------------------------------

TEST(WilderAtr, FirstBarTrueRangeIsItsOwnRange) {
    WilderAtr<1> atr;
    atr.Update(P(110.0), P(100.0), P(105.0));
    ASSERT_TRUE(atr.Ready());
    EXPECT_EQ(atr.Value(), P(10.0));
}

// The two prev_close terms are what make True Range gap-aware. A bar whose
// own high-low is tiny but which opened far from the previous close has a
// LARGE true range - dropping those terms understates volatility exactly when
// it matters most.
TEST(WilderAtr, TrueRangeAccountsForAGapAgainstPreviousClose) {
    WilderAtr<1> atr;
    atr.Update(P(101.0), P(100.0), P(100.0));  // seeds prev_close = 100
    // Next bar's own range is 1.0, but it is 50 above the previous close.
    atr.Update(P(151.0), P(150.0), P(150.0));
    EXPECT_EQ(atr.Value(), P(51.0));  // 151 - 100, not 1.0
}

TEST(WilderAtr, SeedsWithMeanOfFirstPeriodTrueRanges) {
    WilderAtr<4> atr;
    // Flat closes so every TR is exactly the bar's own range.
    atr.Update(P(102.0), P(100.0), P(101.0));
    EXPECT_FALSE(atr.Ready());
    atr.Update(P(103.0), P(101.0), P(101.0));
    atr.Update(P(104.0), P(102.0), P(101.0));
    atr.Update(P(105.0), P(103.0), P(101.0));
    ASSERT_TRUE(atr.Ready());
    EXPECT_GT(atr.Value(), 0);
}

TEST(WilderAtr, ConstantRangeConvergesToThatRange) {
    WilderAtr<14> atr;
    for (int i = 0; i < 500; ++i) {
        atr.Update(P(110.0), P(100.0), P(105.0));
    }
    // prev_close is 105 every bar, so TR = max(10, 5, 5) = 10.
    EXPECT_EQ(atr.Value(), P(10.0));
}

// NATR is the one expression in the system that overflows int64 and needs
// __int128: atr ~1e15 at kPriceScale, times 10000, is ~1e19 against int64's
// 9.2e18. If this returns a nonsense value, the 128-bit intermediate is gone.
TEST(WilderAtr, NatrDoesNotOverflowAtBtcScale) {
    WilderAtr<14> atr;
    for (int i = 0; i < 100; ++i) {
        // ~1% range on a 95k price.
        atr.Update(P(95'475.0), P(94'525.0), P(95'000.0));
    }
    ASSERT_TRUE(atr.Ready());
    const int32_t natr = atr.NatrBp(P(95'000.0));
    // TR = max(950, 475, 475) = 950 -> 950/95000 = 1.0% = 100bp
    EXPECT_NEAR(natr, 100, 1);
}

TEST(WilderAtr, NatrIsScaleInvariant) {
    WilderAtr<14> cheap;
    WilderAtr<14> expensive;
    for (int i = 0; i < 100; ++i) {
        cheap.Update(P(0.0000101), P(0.0000099), P(0.00001));
        expensive.Update(P(101'000.0), P(99'000.0), P(100'000.0));
    }
    // Both are a 2% high-low range around the close, so both NATRs must
    // agree - that is the point of normalising, and of both operands sharing
    // kPriceScale so it cancels.
    EXPECT_NEAR(cheap.NatrBp(P(0.00001)), expensive.NatrBp(P(100'000.0)), 2);
}

TEST(WilderAtr, NatrOnZeroCloseDoesNotDivideByZero) {
    WilderAtr<14> atr;
    for (int i = 0; i < 20; ++i) {
        atr.Update(P(110.0), P(100.0), P(105.0));
    }
    EXPECT_EQ(atr.NatrBp(0), 0);
}

// ---------------------------------------------------------------------------
// TurnoverWindow
// ---------------------------------------------------------------------------

TEST(TurnoverWindow, NotReadyUntilBothWindowsAreFull) {
    TurnoverWindow<24> w;
    for (int i = 0; i < 47; ++i) {
        w.Update(V(1.0));
        EXPECT_FALSE(w.Get().ready) << "ready after " << (i + 1) << " bars";
    }
    w.Update(V(1.0));
    EXPECT_TRUE(w.Get().ready);
}

TEST(TurnoverWindow, SplitsTheTwoWindowsCorrectly) {
    TurnoverWindow<3> w;
    // Oldest first: prev window should end up {1,2,3}, recent {4,5,6}.
    for (int64_t i = 1; i <= 6; ++i) {
        w.Update(V(static_cast<double>(i)));
    }
    const auto v = w.Get();
    ASSERT_TRUE(v.ready);
    EXPECT_EQ(v.prev, V(1.0 + 2.0 + 3.0));
    EXPECT_EQ(v.recent, V(4.0 + 5.0 + 6.0));
}

TEST(TurnoverWindow, SlidesAndDropsTheOldestBar) {
    TurnoverWindow<2> w;
    w.Update(V(1.0));
    w.Update(V(2.0));
    w.Update(V(3.0));
    w.Update(V(4.0));
    // prev {1,2}, recent {3,4}
    EXPECT_EQ(w.Get().prev, V(3.0));
    EXPECT_EQ(w.Get().recent, V(7.0));

    w.Update(V(5.0));
    // prev {2,3}, recent {4,5} - the 1 left entirely
    EXPECT_EQ(w.Get().prev, V(5.0));
    EXPECT_EQ(w.Get().recent, V(9.0));
}

// 24 bars of BTC-scale turnover is ~2.4e16 at kVolumeScale. This is the value
// that crosses double's exact-integer limit of 2^53 ~ 9.0e15 - which is why
// the sums are int64 and why `double GetValue()` could never have carried
// them.
TEST(TurnoverWindow, SumsBtcScaleTurnoverExactly) {
    TurnoverWindow<24> w;
    const Volume per_bar = V(1e9);  // 1 billion USDT per hour
    for (int i = 0; i < 48; ++i) {
        w.Update(per_bar);
    }
    const auto v = w.Get();
    ASSERT_TRUE(v.ready);
    EXPECT_EQ(v.recent, per_bar * 24);
    EXPECT_EQ(v.prev, per_bar * 24);
    // Past 2^53: the assertion above is only meaningful if we are actually in
    // the range where a double would have lost precision.
    EXPECT_GT(v.recent, (int64_t{1} << 53));
}

TEST(TurnoverWindow, ResetClearsBothSums) {
    TurnoverWindow<2> w;
    for (int i = 0; i < 4; ++i) {
        w.Update(V(1.0));
    }
    ASSERT_TRUE(w.Get().ready);
    w.Reset();
    const auto v = w.Get();
    EXPECT_FALSE(v.ready);
    EXPECT_EQ(v.recent, 0);
    EXPECT_EQ(v.prev, 0);
}
