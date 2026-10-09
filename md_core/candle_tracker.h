#pragma once

// CandleTracker: the candle state and indicators for ONE timeframe.
//
// Why the decomposition is per timeframe and not per filter condition:
// every indicator is driven by exactly one timeframe. The 24-bar turnover
// windows and Wilder's ATR are fed by closed 1h bars; EMA50 is fed by
// aggregated 4h bars. Nothing is fed by both. (DESIGN.md §6 records the
// three-signal-manager alternative and why this replaced it: the trend
// condition spans the two timeframes, so a TrendManager needed UpdateLtf()
// AND UpdateHtf() - a class with two update paths keyed by timeframe is SSSSSSS
// class fighting its own decomposition.)
//
// The three CONDITIONS are evaluated in CoreManager, which reads values from
// both trackers. Trend is the only one that spans them, and it spans as two
// scalar reads: the LTF's last close against the HTF's EMA.

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>

#include "md_core/indicators/ema.h"
#include "md_core/indicators/turnover_window.h"
#include "md_core/indicators/wilder_atr.h"
#include "types/candle.h"

namespace screener {

inline constexpr int kEmaPeriod = 50;
inline constexpr int kAtrPeriod = 14;
inline constexpr int kSurgeWindowBars = 24;

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
    static CandleTrackerPtr MakeLtf() {  // H1: volatility + surge
        return CandleTrackerPtr(new CandleTracker(TimeFrame::kH1, Use::kTurnover | Use::kAtr));
    }
    static CandleTrackerPtr MakeHtf() {  // H4: trend
        return CandleTrackerPtr(new CandleTracker(TimeFrame::kH4, Use::kEma));
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

        if (turnover_) {
            turnover_->Update(c.turnover);
        }
        if (atr_) {
            atr_->Update(c.high, c.low, c.close);
        }
        if (ema_) {
            ema_->Update(c.close);
        }
    }

    // A gap cannot be patched into a recursive indicator, so recovery is a
    // full reset plus a replay of REST history. Warm-up uses the same path:
    // Reset() then Apply() per bar, oldest first.
    void Reset() {
        if (turnover_) {
            turnover_->Reset();
        }
        if (atr_) {
            atr_->Reset();
        }
        if (ema_) {
            ema_->Reset();
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

    bool HasAtr() const noexcept { return atr_.has_value(); }
    bool AtrReady() const noexcept { return atr_ && atr_->Ready(); }
    int32_t GetNatrBp() const noexcept { return atr_->NatrBp(last_close_); }

    bool HasTurnover() const noexcept { return turnover_.has_value(); }
    TurnoverWindow<kSurgeWindowBars>::Value Turnover() const noexcept {
        return turnover_ ? turnover_->Get() : TurnoverWindow<kSurgeWindowBars>::Value{false, 0, 0};
    }

   private:
    enum Use : uint8_t {
        kEma = 1,
        kAtr = 2,
        kTurnover = 4
    };

    explicit CandleTracker(TimeFrame frame, uint8_t use) : frame_(frame) {
        if (use & kEma) ema_.emplace();
        if (use & kAtr) atr_.emplace();
        if (use & kTurnover) turnover_.emplace();
    }

    TimeFrame frame_;
    std::optional<Ema<kEmaPeriod>> ema_;
    std::optional<WilderAtr<kAtrPeriod>> atr_;
    std::optional<TurnoverWindow<kSurgeWindowBars>> turnover_;

    std::deque<Candle> recent_;
    Price last_close_ = 0;
    uint32_t bar_count_{};
};

}  // namespace screener
