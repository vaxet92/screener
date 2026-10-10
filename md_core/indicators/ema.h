#pragma once

// Ema<Period>: exponential moving average over scaled-integer prices.
//
// Used for ONE thing: EMA50 on the HTF (4h) close, which the trend condition
// compares the latest LTF close against.

#include <cassert>
#include <cstdint>

#include "md_core/indicators/int_math.h"
#include "types/candle.h"

namespace screener {

class Ema {
   public:
    // `period` was a template parameter (Period); it is now a runtime value
    // so it can come from config.json. The precondition moves with it: a
    // static_assert can no longer check a value that isn't known until
    // construction.
    explicit Ema(int period) noexcept : period_(period) { assert(period_ > 1); }

    // Feed one CLOSED bar's close. Values are at kPriceScale.
    void Update(Price close) noexcept {
        ++bars_;
        if (bars_ <= static_cast<uint32_t>(period_)) {
            // Seed with the simple mean of the first `period_` closes. An EMA
            // seeded from its first sample alone carries that one bar's noise
            // for ~period_ bars; the SMA seed is the standard choice and makes
            // the value comparable to what a charting package shows.
            seed_sum_ += close;
            if (bars_ == static_cast<uint32_t>(period_)) {
                value_ = RoundedDiv(seed_sum_, period_);
            }
            return;
        }
        // value += (close - value) * 2 / (period_ + 1)
        //
        // No overflow: close and value are both <= ~1e15 (BTC at kPriceScale),
        // so the delta is <= ~1e15 and x2 is ~2e15, well inside int64's 9.2e18.
        // This is why the EMA recursion needs no __int128 while NATR does.
        const int64_t delta = close - value_;
        value_ += RoundedDiv(delta * 2, period_ + 1);
    }

    // "Has seen enough history", which is NOT the same question as "has been
    // constructed". A symbol whose EMA is not ready is `not ready` and can
    // never go ACTIVE.
    bool Ready() const noexcept { return bars_ >= static_cast<uint32_t>(period_); }

    // Meaningless unless Ready(). Returns a VALUE, never a verdict - the
    // thresholds live in CoreManager.
    Price Value() const noexcept { return value_; }

    uint32_t Bars() const noexcept { return bars_; }

    // A gap cannot be patched into a recursive indicator, so recovery is a
    // full reset plus a replay from REST. Same path as warm-up. `period_` is
    // NOT reset - it is a construction-time fact, not indicator state.
    void Reset() noexcept {
        value_ = 0;
        seed_sum_ = 0;
        bars_ = 0;
    }

   private:
    int period_;
    Price value_ = 0;
    int64_t seed_sum_ = 0;
    uint32_t bars_ = 0;
};

}  // namespace screener
