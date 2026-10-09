#pragma once

// Ema<Period>: exponential moving average over scaled-integer prices.
//
// Used for ONE thing: EMA50 on the HTF (4h) close, which the trend condition
// compares the latest LTF close against.

#include <cstdint>

#include "md_core/indicators/int_math.h"
#include "types/candle.h"

namespace screener {

template <int Period>
class Ema {
    static_assert(Period > 1, "Period must be > 1");

   public:
    // Feed one CLOSED bar's close. Values are at kPriceScale.
    void Update(Price close) noexcept {
        ++bars_;
        if (bars_ <= Period) {
            // Seed with the simple mean of the first `Period` closes. An EMA
            // seeded from its first sample alone carries that one bar's noise
            // for ~Period bars; the SMA seed is the standard choice and makes
            // the value comparable to what a charting package shows.
            seed_sum_ += close;
            if (bars_ == Period) {
                value_ = RoundedDiv(seed_sum_, Period);
            }
            return;
        }
        // value += (close - value) * 2 / (Period + 1)
        //
        // No overflow: close and value are both <= ~1e15 (BTC at kPriceScale),
        // so the delta is <= ~1e15 and x2 is ~2e15, well inside int64's 9.2e18.
        // This is why the EMA recursion needs no __int128 while NATR does.
        const int64_t delta = close - value_;
        value_ += RoundedDiv(delta * 2, Period + 1);
    }

    // "Has seen enough history", which is NOT the same question as "has been
    // constructed". A symbol whose EMA is not ready is `not ready` and can
    // never go ACTIVE.
    bool Ready() const noexcept { return bars_ >= Period; }

    // Meaningless unless Ready(). Returns a VALUE, never a verdict - the
    // thresholds live in CoreManager.
    Price Value() const noexcept { return value_; }

    uint32_t Bars() const noexcept { return bars_; }

    // A gap cannot be patched into a recursive indicator, so recovery is a
    // full reset plus a replay from REST. Same path as warm-up.
    void Reset() noexcept {
        value_ = 0;
        seed_sum_ = 0;
        bars_ = 0;
    }

   private:
    Price value_ = 0;
    int64_t seed_sum_ = 0;
    uint32_t bars_ = 0;
};

}  // namespace screener
