#pragma once

// CoreManager: owns every symbol's state, applies closed candles to it, and
// decides ACTIVE / INACTIVE.
//
// This is where the THRESHOLDS live. The indicators return values; the
// comparisons that turn a value into a verdict are here, in one place, so
// changing a threshold never means touching an indicator.
//
// THREADING: single thread. The provider calls ApplyCandle() directly from
// the WebSocket read handler, on the one io_context thread, and that direct
// call is safe ONLY because there is no other thread. The moment parsing
// moves off this thread it becomes a data race on every indicator, and the
// callback must become an SPSC push (DESIGN.md §13, Phase 1 item 10).

#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "md_core/symbol_tracker.h"
#include "types/candle.h"

namespace screener {

class CoreManager {
   public:
    // What the four conditions said for one symbol. Returned (rather than
    // collapsed to a bool) so a transition can be logged with its reason - a
    // wrong verdict is near-impossible to diagnose from "INACTIVE" alone.
    struct Conditions {
        bool ready = false;       // every indicator has enough history
        bool surge = false;       // recent turnover >> previous turnover
        bool trend = false;       // last 1h close above the 4h EMA
        bool volatility = false;  // NATR above the floor
        bool liquidity = false;   // recent-window turnover above the floor

        bool All() const noexcept { return ready && surge && trend && volatility && liquidity; }
    };

    // Thresholds moved out of compile-time constants (DESIGN.md §15) so
    // config.json can set them. Integer comparisons only - see DESIGN.md §3.
    //
    //   ema_period          - HTF EMA period (was Ema<kEmaPeriod>)
    //   surge_numerator/denominator - recent*denominator > prev*numerator
    //   min_natr_bp         - NATR floor, in basis points
    //   min_turnover        - liquidity floor, at kVolumeScale, on the same
    //                         recent-window sum the surge condition reads
    explicit CoreManager(uint32_t ema_period, int64_t surge_numerator, int64_t surge_denominator,
                         int32_t min_natr_bp, Volume min_turnover) noexcept
        : ema_period_(ema_period),
          surge_numerator_(surge_numerator),
          surge_denominator_(surge_denominator),
          min_natr_bp_(min_natr_bp),
          min_turnover_(min_turnover) {}

    uint32_t EmaPeriod() const noexcept { return ema_period_; }

    // Builds the symbol universe. Called once at startup, before any candle.
    // The returned ids are indices into the state vector, and a Candle
    // carries one - so dispatch on the message path is an array index, with
    // no symbol string and no hash lookup involved.
    void SetUniverse(const std::vector<Symbol>& symbols);

    std::size_t SymbolCount() const noexcept { return state_.size(); }

    // nullptr if unknown. Startup only - never on the message path.
    const uint32_t* FindId(const Symbol& symbol) const;

    const Symbol& NameOf(uint32_t symbol_id) const { return state_[symbol_id].Name(); }

    // Replace a symbol's entire history: reset the indicators, then replay
    // `candles` oldest-first. This is BOTH the warm-up path and the gap
    // recovery path, deliberately - one code path means warm-up state and
    // live state cannot drift apart.
    //
    // `candles` MUST be oldest-first. Bybit's REST kline returns newest-first,
    // so the caller reverses it; feeding it as received runs the recursive
    // indicators backwards and produces values that look plausible and are
    // wrong.
    void Warmup(uint32_t symbol_id, std::span<const Candle> candles);

    // Apply one CLOSED 1h candle. The only entry point on the message path.
    void ApplyCandle(const Candle& c);

    // Symbols that hit a gap and need a REST rebuild. Drained by the main
    // loop after each frame, never serviced inline - see RequestRebuild().
    std::vector<uint32_t> TakePendingRebuilds();

    // One ACTIVE/INACTIVE edge. `active` is the state the symbol just
    // entered, not merely "something changed" - the notifier formats a
    // different message for each direction.
    struct Transition {
        uint32_t symbol_id;
        bool active;
    };

    // Transitions since the last drain. Swap-out, same shape as
    // TakePendingRebuilds() - drained by ControlManager::DrainRebuilds,
    // which already runs after ApplyCandle has returned. Formatting a chat
    // message and URL-encoding it from inside TryActivate would put network
    // I/O on the message path, so this only RECORDS the edge; sending it is
    // the caller's job.
    std::vector<Transition> TakeTransitions();

    const std::unordered_set<Symbol>& ActiveInstruments() const noexcept { return active_instruments_; }

    // Evaluate without mutating, for tests and for the console table.
    Conditions EvaluateConditions(uint32_t symbol_id) const;

    const SymbolTracker& TrackerAt(uint32_t symbol_id) const { return state_[symbol_id]; }

   private:
    // Hook for "a candle on this timeframe just closed". Fires once per
    // closed 1h bar, and additionally for the 4h bar that bar completes.
    void ApplyCandleComplete(SymbolTracker& s, TimeFrame frame);

    void Evaluate(SymbolTracker& s);
    void TryActivate(SymbolTracker& s, const Conditions& c);
    void TryDeactivate(SymbolTracker& s, const Conditions& c);

    // Record the symbol for a later rebuild and return.
    //
    // NOT inline. A rebuild is a blocking REST fetch followed by a replay of
    // ~600 bars through the warm-up path - so servicing it from inside
    // ApplyCandle would re-enter the state this call is in the middle of
    // reading, 600 times, while the outer call is still on the stack.
    // Recording the id and letting the main loop drain it afterwards costs a
    // push_back and removes the re-entrancy entirely.
    void RequestRebuild(SymbolTracker& s);

    const uint32_t ema_period_;
    const int64_t surge_numerator_;
    const int64_t surge_denominator_;
    const int32_t min_natr_bp_;
    const Volume min_turnover_;

    std::vector<SymbolTracker> state_;

    // Startup and logging only. The message path uses Candle::symbol_id.
    std::unordered_map<Symbol, uint32_t> id_by_symbol_;

    // Just the names, as asked: nothing reads it on the per-bar path, it is
    // touched only on a transition, and it is what the console table prints.
    std::unordered_set<Symbol> active_instruments_;

    std::vector<uint32_t> pending_rebuilds_;
    std::vector<Transition> pending_transitions_;
};

}  // namespace screener
