#pragma once

// FastTurnoverTracker: the same 24h-vs-24h turnover window the surge and
// liquidity conditions have always read, but driven by closed 5-MINUTE bars
// instead of closed 1h bars (DESIGN.md §16).
//
// Kept as its own narrow type rather than squeezed into CandleTracker's Use
// bitmask: CandleTracker's turnover_ member is hardcoded to
// TurnoverWindow<kSurgeWindowBars> (24 x 1h bars == 24h), and this window
// needs a DIFFERENT size (288 x 5m bars == 24h) - same shape, different
// template argument. Templatizing CandleTracker on window size for a field
// only this caller will ever use is more machinery than a second small type.
//
// It also tracks the latest close, because the trend condition's price leg
// now reads the most recently closed bar on WHICHEVER stream is freshest -
// which, between two closed 1h bars, is this one (DESIGN.md §16). The EMA50
// itself is untouched: it is still fed only by closed 4h bars.

#include <cstdint>

#include "md_core/indicators/turnover_window.h"
#include "types/candle.h"

namespace screener {

// 24h of 5-minute bars.
inline constexpr int kFastSurgeWindowBars = 288;

class FastTurnoverTracker {
   public:
    void Apply(const Candle& c) noexcept {
        turnover_.Update(c.turnover);
        last_close_ = c.close;
        ++bar_count_;
    }

    // A gap on this stream only ever invalidates THIS sum - it is not fed
    // into a recursive indicator, so re-summing from REST is enough. See
    // CoreManager::RequestFastRebuild for why that gap rule is lighter than
    // the one a 1h gap gets.
    void Reset() noexcept {
        turnover_.Reset();
        last_close_ = 0;
        bar_count_ = 0;
    }

    uint32_t Bars() const noexcept { return bar_count_; }
    Price LastClose() const noexcept { return last_close_; }

    TurnoverWindow<kFastSurgeWindowBars>::Value Turnover() const noexcept { return turnover_.Get(); }

   private:
    TurnoverWindow<kFastSurgeWindowBars> turnover_;
    Price last_close_ = 0;
    uint32_t bar_count_ = 0;
};

}  // namespace screener
