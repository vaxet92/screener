#pragma once

// Integer helpers shared by the indicators. No floating point anywhere in
// this system, including here - see DESIGN.md §3.

#include <cstdint>

namespace screener {

// Divide with round-half-away-from-zero, for a POSITIVE divisor.
//
// Why rounding and not C's truncation: both the EMA and Wilder's ATR are
// RECURSIVE - this quarter's output is next quarter's input. Truncation always
// biases the same direction, so the error does not cancel, it accumulates.
// Over the 600-bar warm-up an EMA50 built with truncating division drifts
// visibly below a floating-point reference. Rounding leaves a bounded error
// (~13 units at kPriceScale, ~0.02% for the cheapest symbol) that does not
// walk, which is why the indicator tests compare against a double reference
// with a stated tolerance instead of demanding equality.
//
// Away-from-zero, not toward-even: the EMA delta is signed, and a rule that
// treats +0.5 and -0.5 asymmetrically would introduce exactly the directional
// bias this exists to remove.
constexpr int64_t RoundedDiv(int64_t numerator, int64_t divisor) noexcept {
    const int64_t half = divisor / 2;
    return numerator >= 0 ? (numerator + half) / divisor : -((-numerator + half) / divisor);
}

// Same, for the one place a 128-bit intermediate is unavoidable (NATR).
constexpr int64_t RoundedDiv128(__int128 numerator, __int128 divisor) noexcept {
    const __int128 half = divisor / 2;
    const __int128 q = numerator >= 0 ? (numerator + half) / divisor : -((-numerator + half) / divisor);
    return static_cast<int64_t>(q);
}

}  // namespace screener
