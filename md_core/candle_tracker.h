#pragma once

// CandleTracker: the candle state and indicators for ONE timeframe.
//
// Why the decomposition is per timeframe and not per filter condition:
// every indicator is driven by exactly one timeframe. Wilder's ATR is fed by
// closed 1h bars; EMA50 is fed by aggregated 4h bars. Nothing is fed by both.
// (DESIGN.md §6 records the three-signal-manager alternative and why this
// replaced it: the trend condition spans the two timeframes, so a
// TrendManager needed UpdateLtf() AND UpdateHtf() - a class with two update
// paths keyed by timeframe is a class fighting its own decomposition.)
//
// The 24-bar turnover SUM window that used to live on THIS tracker (surge and
// liquidity) moved to FastTurnoverTracker, fed by a THIRD, independently
// subscribed 5m stream (DESIGN.md §16) - it is not an aggregate of anything
// this class owns, so it is not a third CandleTracker factory either.
//
// The liquidity condition itself later moved back to the LTF side in the
// shape of a MEDIAN of the last 24 hourly turnovers (DESIGN.md §17) - a
// different question from the fast tracker's sum, and naturally hourly, so
// it lives here as a third optional indicator rather than on the 5m stream.
//
// The CONDITIONS are evaluated in CoreManager, which reads values from all
// three trackers. Trend is the one that spans timeframes, and it spans as
// two scalar reads: the fast tracker's last close against the HTF's EMA.

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>

#include "md_core/indicators/ema.h"
#include "md_core/indicators/turnover_median_window.h"
#include "md_core/indicators/wilder_atr.h"
#include "types/candle.h"

namespace screener {

inline constexpr int kEmaPeriod = 50;
inline constexpr int kAtrPeriod = 14;

// How many closed candles to keep per timeframe. Inspection and debugging
// only - no condition reads them. Kept because a wrong ACTIVE verdict is
// almost impossible to diagnose without seeing the bars that produced it.
inline constexpr std::size_t kRecentCandles = 10;

class CandleTracker;
using CandleTrackerPtr = std::unique_ptr<CandleTracker>;

class CandleTracker {
   public:
    // Two factories instead of a public constructor, so the "which indicators
    // does this timeframe have" decision lives in exactly one place rather
    // than at every construction site.
    //
    // `new` + the unique_ptr constructor, NOT std::make_unique: make_unique is
    // a free function template, not a friend, so it constructs the object from
    // outside the class and cannot reach a private constructor. These factories
    // are members, so they can. The raw `new` is handed to a unique_ptr on the
    // same line, so there is no window in which it could leak.
    // H1: volatility, plus the median-turnover liquidity window (DESIGN.md
    // §17). The SUM turnover window that used to live here moved to
    // FastTurnoverTracker, on the 5m stream (DESIGN.md §16) - surge and
    // trend's price leg read that one; liquidity reads this one.
    static CandleTrackerPtr MakeLtf() {
        return CandleTrackerPtr(new CandleTracker(TimeFrame::kH1, Use::kAtr | Use::kMedianTurnover, 0, 0));
    }
    // `slow_period`/`fast_period` are runtime now (config.json), not the
    // kEmaPeriod constant - only meaningful here, since MakeLtf's timeframe
    // never uses Ema. `fast_period` feeds the fast/slow EMA crossover
    // condition (CoreManager::EvaluateConditions) - must stay below
    // `slow_period`, enforced in ScreenerConfig::Validate(), not here: a
    // CandleTracker has no config to validate against.
    static CandleTrackerPtr MakeHtf(int slow_period, int fast_period) {  // H4: trend
        return CandleTrackerPtr(new CandleTracker(TimeFrame::kH4, Use::kEma, slow_period, fast_period));
    }

    // Copy deleted, move defaulted.
    //
    // Deleting the copy is right: duplicating recursive indicator state is
    // never meaningful, and a silent copy would give two trackers that drift
    // apart. But declaring ANY copy operation also suppresses the implicit
    // MOVE constructor, and without a move SymbolTracker is neither copyable
    // nor movable - which makes std::vector<SymbolTracker> fail to compile on
    // `Cpp17MoveInsertable`, because a vector must be able to relocate its
    // elements when it grows.
    CandleTracker(const CandleTracker&) = delete;
    CandleTracker& operator=(const CandleTracker&) = delete;
    CandleTracker(CandleTracker&&) = default;
    CandleTracker& operator=(CandleTracker&&) = default;

    void Apply(const Candle& c) {
        ++bar_count_;
        last_close_ = c.close;

        recent_.push_back(c);
        if (recent_.size() > kRecentCandles) {
            recent_.pop_front();
        }

        if (atr_) {
            atr_->Update(c.high, c.low, c.close);
        }
        if (ema_) {
            ema_->Update(c.close);
        }
        if (fast_ema_) {
            fast_ema_->Update(c.close);
        }
        if (median_turnover_) {
            median_turnover_->Update(c.turnover);
        }
    }

    // A gap cannot be patched into a recursive indicator, so recovery is a
    // full reset plus a replay of REST history. Warm-up uses the same path:
    // Reset() then Apply() per bar, oldest first.
    void Reset() {
        if (atr_) {
            atr_->Reset();
        }
        if (ema_) {
            ema_->Reset();
        }
        if (fast_ema_) {
            fast_ema_->Reset();
        }
        if (median_turnover_) {
            median_turnover_->Reset();
        }
        recent_.clear();
        last_close_ = 0;
        bar_count_ = 0;
    }

    TimeFrame Frame() const noexcept { return frame_; }
    uint32_t Bars() const noexcept { return bar_count_; }

    // A stored member rather than recent_.back().close.
    //
    // recent_ is EMPTY after Reset() and before the first Apply(), so
    // recent_.back() would be undefined behaviour there - and Reset() is
    // exactly what a gap rebuild calls. A deque's back() on an empty deque
    // does not throw, it reads past the end, so this would be a silent
    // garbage read rather than a crash.
    Price LastClose() const noexcept { return last_close_; }

    const std::deque<Candle>& Recent() const noexcept { return recent_; }

    // has_value() answers "does this timeframe use the indicator at all".
    // Ready() answers "has it been fed enough history to mean anything".
    // Two different questions: an EMA50 exists from construction and is
    // meaningless for its first 49 bars, and it is Ready() that decides
    // whether a symbol is `not ready`.
    bool HasEma() const noexcept { return ema_.has_value(); }
    bool EmaReady() const noexcept { return ema_ && ema_->Ready(); }
    Price GetEma() const noexcept { return ema_->Value(); }

    // Fast/slow crossover condition's fast leg. Same has_value()/Ready() split
    // as the slow EMA above, and for the same reason: HasFastEma() is "does
    // this timeframe use it", FastEmaReady() is "has it seen enough bars".
    bool HasFastEma() const noexcept { return fast_ema_.has_value(); }
    bool FastEmaReady() const noexcept { return fast_ema_ && fast_ema_->Ready(); }
    Price GetFastEma() const noexcept { return fast_ema_->Value(); }

    bool HasAtr() const noexcept { return atr_.has_value(); }
    bool AtrReady() const noexcept { return atr_ && atr_->Ready(); }
    int32_t GetNatrBp() const noexcept { return atr_->NatrBp(last_close_); }

    // Liquidity's source (DESIGN.md §17) - same has_value()/Ready() split as
    // every other indicator here.
    bool HasMedianTurnover() const noexcept { return median_turnover_.has_value(); }
    bool MedianTurnoverReady() const noexcept { return median_turnover_ && median_turnover_->Ready(); }
    Volume GetMedianTurnover() const noexcept { return median_turnover_->Median(); }

   private:
    enum Use : uint8_t {
        kEma = 1,
        kAtr = 2,
        kMedianTurnover = 4,
    };

    // `slow_period`/`fast_period` are unused unless `use & kEma` - MakeLtf
    // passes 0 for both.
    explicit CandleTracker(TimeFrame frame, uint8_t use, int slow_period, int fast_period) : frame_(frame) {
        if (use & kEma) {
            ema_.emplace(slow_period);
            fast_ema_.emplace(fast_period);
        }
        if (use & kAtr) atr_.emplace();
        if (use & kMedianTurnover) median_turnover_.emplace();
    }

    TimeFrame frame_;
    std::optional<Ema> ema_;
    std::optional<Ema> fast_ema_;
    std::optional<WilderAtr<kAtrPeriod>> atr_;
    std::optional<TurnoverMedianWindow> median_turnover_;

    std::deque<Candle> recent_;
    Price last_close_ = 0;
    uint32_t bar_count_{};
};

}  // namespace screener
