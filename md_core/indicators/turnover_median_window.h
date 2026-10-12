#pragma once

// TurnoverMedianWindow: the median of the last 24 CLOSED HOURLY turnovers.
//
// Exists because a flat 24h SUM (TurnoverWindow) is fooled by a single spike
// hour: one huge hour can push the sum over a floor while the symbol is
// otherwise dead the rest of the day. The median ignores that one outlier
// bar, so it answers "is this symbol liquid most of the time", not "did it
// move a lot at some point today".
//
// Caches the median on Update() rather than recomputing on Median(): this
// window is fed by the LTF (1h) stream, but CoreManager::EvaluateConditions
// reads it on EVERY closed bar on EITHER stream (the fast 5m one too), so
// Median() must be an O(1) read.

#include <algorithm>
#include <array>
#include <cstddef>
#include <deque>

#include "types/candle.h"

namespace screener {

class TurnoverMedianWindow {
   public:
    void Update(Volume turnover) {
        bars_.push_back(turnover);
        if (bars_.size() > kBars) {
            bars_.pop_front();
        }

        if (bars_.size() == kBars) {
            std::array<Volume, kBars> sorted{};
            std::copy(bars_.begin(), bars_.end(), sorted.begin());
            std::sort(sorted.begin(), sorted.end());
            // The lower-middle of 24 sorted values, not an average of the two
            // center ones. This is a one-shot threshold check, not a
            // recursive indicator, so there is nothing for an averaging
            // rounding rule to protect against - and the lower-middle is
            // still one real observed turnover value rather than a number
            // that was never actually seen on the wire.
            median_ = sorted[kLowerMedianIndex];
        }
    }

    bool Ready() const noexcept { return bars_.size() >= kBars; }

    // UB if !Ready() - same contract as WilderAtr/Ema.
    Volume Median() const noexcept { return median_; }

    void Reset() {
        bars_.clear();
        median_ = 0;
    }

   private:
    static constexpr std::size_t kBars = 24;
    static constexpr std::size_t kLowerMedianIndex = kBars / 2 - 1;  // 11: the 12th-smallest of 24

    std::deque<Volume> bars_;
    Volume median_ = 0;
};

}  // namespace screener
