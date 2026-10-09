#pragma once

// WilderAtr<Period>: Wilder's Average True Range over scaled-integer prices,
// plus the NATR (normalised ATR) in basis points that the volatility
// condition actually tests.

#include <cstdint>

#include "md_core/indicators/int_math.h"
#include "types/candle.h"

namespace screener {

template <int Period>
class WilderAtr {
    static_assert(Period > 0, "Period must be > 0");

   public:
    // True Range needs THREE inputs plus retained state, which is why these
    // indicators are not interchangeable behind a single
    // `virtual void Update(double)`:
    //
    //   TR = max(high - low, |high - prev_close|, |low - prev_close|)
    //
    // The prev_close terms are what make TR gap-aware: a bar that opens far
    // from the previous close has a large true range even if its own high-low
    // is tiny. Dropping them understates volatility exactly when it matters.
    void Update(Price high, Price low, Price close) noexcept {
        if (!has_prev_close_) {
            // First bar ever: no previous close, so TR degenerates to the
            // bar's own range. Wilder's own convention.
            prev_close_ = close;
            has_prev_close_ = true;
            Accumulate(high - low);
            return;
        }
        const Price tr = TrueRange(high, low, prev_close_);
        prev_close_ = close;
        Accumulate(tr);
    }

    bool Ready() const noexcept { return bars_ >= static_cast<uint32_t>(Period); }

    Price Value() const noexcept { return atr_; }

    // NATR in basis points: 100 bp == 1.00 %.
    //
    //   natr_bp = atr / close * 10000
    //
    // Both operands are at kPriceScale, so the scale cancels and the result is
    // a plain ratio - no scale juggling, which is half the reason both are at
    // the same scale.
    //
    // __int128 IS REQUIRED HERE. atr can reach ~1e15 (BTC at kPriceScale) and
    // 1e15 * 10000 = 1e19, past int64's 9.2e18. This is the one indicator
    // expression that overflows; the EMA and ATR recursions do not.
    int32_t NatrBp(Price close) const noexcept {
        if (close <= 0) {
            return 0;
        }
        const __int128 scaled = static_cast<__int128>(atr_) * 10'000;
        return static_cast<int32_t>(RoundedDiv128(scaled, close));
    }

    void Reset() noexcept {
        atr_ = 0;
        seed_sum_ = 0;
        bars_ = 0;
        prev_close_ = 0;
        has_prev_close_ = false;
    }

   private:
    static constexpr Price TrueRange(Price high, Price low, Price prev_close) noexcept {
        const Price a = high - low;
        const Price b = high > prev_close ? high - prev_close : prev_close - high;
        const Price c = low > prev_close ? low - prev_close : prev_close - low;
        return a > b ? (a > c ? a : c) : (b > c ? b : c);
    }

    void Accumulate(Price tr) noexcept {
        ++bars_;
        if (bars_ <= static_cast<uint32_t>(Period)) {
            seed_sum_ += tr;
            if (bars_ == static_cast<uint32_t>(Period)) {
                atr_ = RoundedDiv(seed_sum_, Period);
            }
            return;
        }
        // Wilder smoothing: atr = (atr * (Period - 1) + tr) / Period
        //
        // NOT the same as an EMA with alpha = 2/(P+1) - Wilder's alpha is
        // 1/P. Using the EMA form here would give a visibly different ATR
        // from every charting package, which is the kind of mismatch that
        // reads as a bug in review.
        //
        // No overflow: atr is a price DIFFERENCE, bounded by the price
        // (~1e15), so atr * 13 is ~1.3e16 - inside int64.
        atr_ = RoundedDiv(atr_ * (Period - 1) + tr, Period);
    }

    Price atr_ = 0;
    int64_t seed_sum_ = 0;
    uint32_t bars_ = 0;
    Price prev_close_ = 0;
    bool has_prev_close_ = false;
};

}  // namespace screener
