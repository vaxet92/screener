#pragma once

// SymbolTracker: everything the screener knows about one instrument.
//
// Owns the two CandleTrackers (1h, 4h), the LTF->HTF aggregator, and the
// dedup/gap state. It does NOT own the filter thresholds or the ACTIVE
// decision - those are CoreManager's, so that "what the indicators say" and
// "what we do about it" stay separable.
//
// It also does NOT fetch history. Warm-up and rebuild are REST, and REST
// belongs to ControlManager (DESIGN.md §2): md_core never talks to a venue.
// This class exposes Reset() + ApplyLtf(), and whoever has the history feeds
// it in oldest-first.

#include <cstdint>
#include <optional>
#include <utility>

#include "md_core/candle_tracker.h"
#include "md_core/htf_aggregator.h"
#include "types/candle.h"

namespace screener {

// How a newly arrived bar relates to the last one we accepted.
//
// `open_time` is the ONLY identity of a bar. The envelope ts describes the
// message, not the bar, and two messages for the same bar (a redundant
// delivery, a replayed frame) carry different ts values - keying on it would
// make a duplicate look new.
enum class Arrival {
    kFirst,      // nothing accepted yet
    kNext,       // exactly one LTF period after the last - the only case we apply
    kDuplicate,  // same open_time as the last accepted bar
    kStale,      // older than the last accepted bar
    kGap,        // more than one period ahead: bars are missing
};

class SymbolTracker {
   public:
    SymbolTracker(uint32_t id, Symbol symbol, int ema_period)
        : id_(id),
          symbol_(std::move(symbol)),
          ltf_(CandleTracker::MakeLtf()),
          htf_(CandleTracker::MakeHtf(ema_period)) {}

    uint32_t Id() const noexcept { return id_; }
    const Symbol& Name() const noexcept { return symbol_; }

    Arrival Classify(int64_t open_time_ms) const noexcept {
        if (last_ltf_open_time_ == 0) {
            return Arrival::kFirst;
        }
        if (open_time_ms == last_ltf_open_time_) {
            return Arrival::kDuplicate;
        }
        if (open_time_ms < last_ltf_open_time_) {
            return Arrival::kStale;
        }
        if (open_time_ms == last_ltf_open_time_ + kLtfMs) {
            return Arrival::kNext;
        }
        return Arrival::kGap;
    }

    // Apply one CLOSED 1h candle. Caller must have classified it as kFirst or
    // kNext; a gap must go through Reset() and a replay instead, because the
    // EMA and ATR are recursive and a missing bar cannot be patched in later.
    //
    // Returns the completed 4h candle when this bar closed one, so the caller
    // knows an HTF boundary was crossed. The HTF side is updated HERE, before
    // the caller evaluates the filter - see CoreManager::ApplyCandle for why
    // that ordering is not optional.
    std::optional<Candle> ApplyLtf(const Candle& c) {
        last_ltf_open_time_ = c.open_time_ms;
        ltf_->Apply(c);

        std::optional<Candle> htf = agg_.Add(c);
        if (htf) {
            htf_->Apply(*htf);
        }
        return htf;
    }

    void Reset() {
        ltf_->Reset();
        htf_->Reset();
        agg_.Reset();
        last_ltf_open_time_ = 0;
        warmed_up_regime_ = true;
        // is_active_ is deliberately NOT cleared here. Reset() wipes the
        // indicator state; whether the symbol is in CoreManager's active set
        // is CoreManager's business, and clearing the flag without removing
        // it from that set would desynchronise the two.
    }

    // Dereference the pointer; keep returning a reference, not the pointer.
    //
    // `ltf_` being a unique_ptr is storage, not interface. CoreManager asks
    // "what does the 1h side say", never "where does the 1h side live", so
    // handing out `const CandleTrackerPtr&` would leak the storage choice into
    // every call site (`s.Ltf()->GetNatrBp()`) - and changing the member back
    // to by-value would then be a sweep through core.cpp and the tests instead
    // of a two-line change here. A reference also cannot be stored past the
    // call or null-checked, which is exactly what we want callers to do.
    //
    // Neither reference can be null here: both pointers are set in the member
    // init list and nothing reseats them. That is an invariant of THIS class,
    // not of the type - see the note on the members.
    const CandleTracker& Ltf() const noexcept { return *ltf_; }
    const CandleTracker& Htf() const noexcept { return *htf_; }

    int64_t LastLtfOpenTime() const noexcept { return last_ltf_open_time_; }

    bool IsActive() const noexcept { return is_active_; }
    void SetActive(bool active) noexcept { is_active_ = active; }

    // "A gap was seen; a REST rebuild is queued for this symbol."
    //
    // Two jobs. It stops a second gap from queueing the same symbol twice,
    // and it makes ApplyCandle drop bars that arrive in the window between
    // the gap and the rebuild - those bars would be applied to indicator
    // state the rebuild is about to overwrite anyway, and the refetch will
    // contain them.
    bool RebuildPending() const noexcept { return rebuild_pending_; }
    void SetRebuildPending(bool pending) noexcept { rebuild_pending_ = pending; }

   private:
    uint32_t id_;
    Symbol symbol_;  // logging and the active set only - never on the message path

    // unique_ptr, held through CandleTracker's factories.
    //
    // Cost of the indirection, stated so it is a decision and not an accident:
    // two heap allocations per symbol (~1600 across the universe) at startup,
    // and one pointer chase per accessor on the per-bar path. Both are paid
    // once per closed 1h bar, so ~1000 times an HOUR - immaterial here, and
    // Phase 1 can measure it if it ever looks otherwise.
    //
    // What it does cost correctness-wise: a unique_ptr can be null, and after
    // SymbolTracker is moved from, these ARE null while the object is still
    // alive. `ltf_->Apply(...)` on a moved-from tracker is UB, where a
    // by-value member would merely be an empty-but-valid tracker.
    // std::vector<SymbolTracker> moves its elements when it grows, so the
    // rule is: nothing touches a SymbolTracker after moving it. Ltf()/Htf()
    // keep that out of the callers' hands by never exposing the pointer.
    CandleTrackerPtr ltf_;
    CandleTrackerPtr htf_;

    HtfAggregator agg_;

    // Dedup/gap state belongs to the symbol, not to an indicator: it is about
    // bar identity, which every indicator shares.
    int64_t last_ltf_open_time_ = 0;

    bool is_active_ = false;

    bool rebuild_pending_ = false;
    bool warmed_up_regime_ = true;  // set
};

}  // namespace screener
