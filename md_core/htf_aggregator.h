#pragma once

// HtfAggregator: 4 closed LTF (1h) candles -> 1 HTF (4h) candle.
//
// We subscribe to the 1h kline ONLY and build the 4h locally, for warm-up and
// for live data, through this one class. That is what makes a warm-up HTF bar
// and a live HTF bar impossible to disagree: there is no second code path for
// them to disagree with.

#include <optional>

#include "types/candle.h"

namespace screener {

class HtfAggregator {
   public:
    // Returns the completed HTF candle on the 4th bar of a bucket, otherwise
    // nullopt.
    std::optional<Candle> Add(const Candle& ltf) noexcept {
        const int64_t bucket = HtfBucketStart(ltf.open_time_ms);

        if (count_ == 0) {
            // ALIGNMENT GATE. Only start a bucket on its first hour.
            //
            // Warm-up hands us an arbitrary window of history, so the oldest
            // bar is usually mid-bucket. Aggregating it anyway would build the
            // first "4h" candle out of 1-3 bars, and that wrong bar then seeds
            // the EMA50 - which decays over ~50 bars, so the error is largest
            // where nobody looks and invisible by the time anybody does.
            // Dropping the leading partial group is the only safe option: a
            // partial bar cannot be repaired, because the hours it is missing
            // are older than the data we were given.
            if (ltf.open_time_ms != bucket) {
                return std::nullopt;
            }
            acc_ = ltf;
            acc_.open_time_ms = bucket;
            count_ = 1;
            return std::nullopt;
        }

        // A bucket change with count_ != 0 means the previous bucket never
        // completed - only reachable after a gap, which forces a full rebuild
        // anyway. Restart cleanly rather than emit a short bar.
        if (bucket != acc_.open_time_ms) {
            count_ = 0;
            return Add(ltf);
        }

        if (ltf.high > acc_.high) {
            acc_.high = ltf.high;
        }
        if (ltf.low < acc_.low) {
            acc_.low = ltf.low;
        }
        acc_.close = ltf.close;
        acc_.turnover += ltf.turnover;
        // The HTF bar closes when its LAST LTF bar closes, so it inherits that
        // message's envelope ts. open_time_ms stays the bucket start: identity
        // comes from the bar, latency from the message.
        acc_.event_time_ms = ltf.event_time_ms;
        ++count_;

        if (count_ == kLtfBarsPerHtf) {
            count_ = 0;
            return acc_;
        }
        return std::nullopt;
    }

    void Reset() noexcept { count_ = 0; }

    int Pending() const noexcept { return count_; }

   private:
    Candle acc_{};
    int count_ = 0;
};

}  // namespace screener
