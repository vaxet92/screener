#pragma once

// TurnoverWindow<Bars>: two ADJACENT windows of `Bars` closed LTF bars each.
//
// The volume-surge condition asks "is the recent window's turnover much
// higher than the window before it", so this holds 2 x Bars bars and keeps a
// running sum for each half.

#include <cstdint>
#include <deque>

#include "types/candle.h"

namespace screener {

template <int Bars>
class TurnoverWindow {
    static_assert(Bars > 0, "Bars must be > 0");

   public:
    // Get() returns the two RAW SUMS, not a ratio.
    //
    // Deliberate: CoreManager tests `recent * 100 > prev * 130`, an exact
    // integer comparison. Returning a ratio would mean dividing here and
    // multiplying there - throwing away precision only to reconstruct it, and
    // the surge threshold is exactly where two nearly-equal windows decide
    // the answer.
    //
    // It is also why one `virtual double GetValue()` could never have served
    // this indicator: the answer is two numbers, not one.
    struct Value {
        bool ready;
        Volume recent;  // newest `Bars` bars
        Volume prev;    // the `Bars` bars before those
    };

    void Update(Volume turnover) {
        bars_.push_back(turnover);
        recent_sum_ += turnover;

        // The bar that just fell out of the recent window moves into the
        // previous one. Running sums, so a bar is added and removed once each
        // rather than re-summing 48 values per update.
        if (bars_.size() > kHalf) {
            const Volume shifted = bars_[bars_.size() - kHalf - 1];
            recent_sum_ -= shifted;
            prev_sum_ += shifted;
        }
        // And the bar that fell out of the previous window leaves entirely.
        if (bars_.size() > kFull) {
            prev_sum_ -= bars_.front();
            bars_.pop_front();
        }
    }

    bool Ready() const noexcept { return bars_.size() >= kFull; }

    Value Get() const noexcept { return Value{Ready(), recent_sum_, prev_sum_}; }

    void Reset() {
        bars_.clear();
        recent_sum_ = 0;
        prev_sum_ = 0;
    }

   private:
    static constexpr std::size_t kHalf = static_cast<std::size_t>(Bars);
    static constexpr std::size_t kFull = 2 * kHalf;

    // std::deque on purpose: Phase 0 uses ordinary containers so Phase 1 has
    // a baseline to measure a FixedRing<Volume, 48> against (CLAUDE.md).
    std::deque<Volume> bars_;
    Volume recent_sum_ = 0;
    Volume prev_sum_ = 0;
};

}  // namespace screener
