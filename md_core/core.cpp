#include "md_core/core.h"

#include "logger/logger.h"

namespace screener {

void CoreManager::SetUniverse(const std::vector<Symbol>& symbols) {
    state_.clear();
    id_by_symbol_.clear();
    active_instruments_.clear();
    pending_rebuilds_.clear();
    pending_transitions_.clear();

    // reserve() before the loop matters more than it looks: SymbolTracker
    // holds two CandleTrackers with deques inside, so a vector reallocation
    // moves every one of them. Reserving also means the ids handed out below
    // stay valid as indices for the whole run.
    state_.reserve(symbols.size());
    id_by_symbol_.reserve(symbols.size());

    for (const Symbol& symbol : symbols) {
        const auto id = static_cast<uint32_t>(state_.size());
        state_.emplace_back(id, symbol);
        id_by_symbol_.emplace(symbol, id);
    }
}

const uint32_t* CoreManager::FindId(const Symbol& symbol) const {
    const auto it = id_by_symbol_.find(symbol);
    return it == id_by_symbol_.end() ? nullptr : &it->second;
}

void CoreManager::Warmup(uint32_t symbol_id, std::span<const Candle> candles) {
    SymbolTracker& s = state_[symbol_id];

    // Full reset, then replay. Warm-up and gap recovery are the SAME path:
    // the indicators are recursive, so there is no way to splice history into
    // a half-built state, and having one path means warm-up values and live
    // values cannot drift apart.
    s.Reset();

    for (const Candle& c : candles) {
        // Every history bar goes through the aggregator too, so the HTF
        // history is built by exactly the code that builds it live. The
        // aggregator drops the leading partial 4h group itself.
        s.ApplyLtf(c);
    }

    s.SetRebuildPending(false);

    // A rebuilt symbol is re-evaluated immediately rather than waiting for
    // its next live bar: it was forced INACTIVE when the gap was seen, and
    // without this it would stay INACTIVE for up to an hour after its state
    // is already correct again.
    Evaluate(s);
}

void CoreManager::ApplyCandle(const Candle& c) {
    if (c.symbol_id >= state_.size()) {
        Logger::Log(LogLevel::kError, "ApplyCandle: unknown symbol_id {}", c.symbol_id);
        return;
    }

    SymbolTracker& s = state_[c.symbol_id];

    // A rebuild is queued: this bar is in the history we are about to fetch,
    // and applying it to state that is about to be overwritten is wasted work.
    if (s.RebuildPending()) {
        return;
    }

    switch (s.Classify(c.open_time_ms)) {
        case Arrival::kDuplicate:
        case Arrival::kStale:
            // Dedup by open_time makes a replay idempotent. It is also what
            // makes the rebuild safe: a bar that closed during the blocking
            // refetch arrives after it and is dropped here, because the
            // refetch already contained it.
            return;
        case Arrival::kGap:
            RequestRebuild(s);
            return;
        case Arrival::kFirst:
        case Arrival::kNext:
            break;
    }

    // ApplyLtf updates the LTF tracker AND, on a 4h boundary, the HTF
    // tracker - both BEFORE Evaluate() runs below.
    //
    // That ordering is not cosmetic. The trend condition compares the latest
    // 1h close against the 4h EMA50. If the filter ran before the HTF update,
    // then on the one bar in four that closes a 4h candle it would compare a
    // fresh close against a one-period-stale EMA - a wrong verdict once every
    // four hours, which is both rare enough to survive testing and frequent
    // enough to matter.
    const std::optional<Candle> htf = s.ApplyLtf(c);

    ApplyCandleComplete(s, TimeFrame::kH1);
    if (htf) {
        ApplyCandleComplete(s, TimeFrame::kH4);
    }

    Evaluate(s);
}

void CoreManager::ApplyCandleComplete(SymbolTracker& s, TimeFrame frame) {
    // Per-timeframe notification hook. The state is already updated by the
    // time this runs; this is where anything that cares about "a bar on this
    // timeframe just closed" belongs (the replay recorder, per-timeframe
    // counters), rather than being folded into the filter.
    Logger::Log(LogLevel::kDebug, "candle complete {} tf={} bars={}", s.Name(), frame == TimeFrame::kH1 ? "1h" : "4h",
                frame == TimeFrame::kH1 ? s.Ltf().Bars() : s.Htf().Bars());
}

CoreManager::Conditions CoreManager::EvaluateConditions(uint32_t symbol_id) const {
    const SymbolTracker& s = state_[symbol_id];
    const CandleTracker& ltf = s.Ltf();
    const CandleTracker& htf = s.Htf();

    Conditions c;

    const auto turnover = ltf.Turnover();

    // Readiness is per indicator and ANDed here. A symbol missing history is
    // `not ready` and can never be ACTIVE - never "probably fine".
    c.ready = turnover.ready && ltf.AtrReady() && htf.EmaReady();
    if (!c.ready) {
        return c;
    }

    // recent / prev > 130 / 100, rearranged so there is no division.
    //
    // __int128 because the left side reaches ~2.4e16 (24 bars of BTC turnover
    // at kVolumeScale) and the right side multiplies by 130, giving ~3.1e18.
    // That is inside int64's 9.2e18, but by only 3x - and the cost of being
    // wrong is a silent sign flip on the comparison that decides the filter.
    // One 128-bit multiply per closed bar is not a cost worth that risk.
    c.surge = static_cast<__int128>(turnover.recent) * kSurgeDenominator >
              static_cast<__int128>(turnover.prev) * kSurgeNumerator;

    // The one condition that spans both timeframes, and it spans as two
    // scalar reads: the latest 1h close against the 4h EMA50.
    c.trend = ltf.LastClose() > htf.GetEma();

    c.volatility = ltf.GetNatrBp() > kMinNatrBp;

    return c;
}

void CoreManager::Evaluate(SymbolTracker& s) {
    const Conditions c = EvaluateConditions(s.Id());
    if (c.All()) {
        TryActivate(s, c);
    } else {
        TryDeactivate(s, c);
    }
}

void CoreManager::TryActivate(SymbolTracker& s, const Conditions& c) {
    if (s.IsActive()) {
        return;  // edge-triggered: already active, nothing to announce
    }
    s.SetActive(true);
    active_instruments_.insert(s.Name());
    pending_transitions_.push_back({s.Id(), true});
    Logger::Log(LogLevel::kInfo, "ACTIVE   {} close={} ema50={} natr={}bp turnover_recent={} prev={}", s.Name(),
                s.Ltf().LastClose(), s.Htf().GetEma(), s.Ltf().GetNatrBp(), c.surge ? s.Ltf().Turnover().recent : 0,
                c.surge ? s.Ltf().Turnover().prev : 0);
}

void CoreManager::TryDeactivate(SymbolTracker& s, const Conditions& c) {
    if (!s.IsActive()) {
        return;
    }
    s.SetActive(false);
    active_instruments_.erase(s.Name());
    pending_transitions_.push_back({s.Id(), false});
    // Logged with WHICH condition failed. "INACTIVE" on its own is
    // undiagnosable after the fact, and these transitions are the product.
    Logger::Log(LogLevel::kInfo, "INACTIVE {} ready={} surge={} trend={} vol={}", s.Name(), c.ready, c.surge, c.trend,
                c.volatility);
}

void CoreManager::RequestRebuild(SymbolTracker& s) {
    if (s.RebuildPending()) {
        return;
    }

    // Force INACTIVE first. The indicators are now known-wrong, so the
    // symbol must stop being reported as a signal before anything else
    // happens - fail safe, not fail quiet.
    if (s.IsActive()) {
        s.SetActive(false);
        active_instruments_.erase(s.Name());
        // This bypasses TryDeactivate, so the edge is recorded here instead -
        // miss it and chat keeps showing a symbol ACTIVE after a gap has
        // already invalidated it.
        pending_transitions_.push_back({s.Id(), false});
    }

    s.SetRebuildPending(true);
    pending_rebuilds_.push_back(s.Id());

    Logger::Log(LogLevel::kWarning, "gap on {} after open_time={} - rebuild queued", s.Name(), s.LastLtfOpenTime());
}

std::vector<uint32_t> CoreManager::TakePendingRebuilds() {
    std::vector<uint32_t> out;
    out.swap(pending_rebuilds_);
    return out;
}

std::vector<CoreManager::Transition> CoreManager::TakeTransitions() {
    std::vector<Transition> out;
    out.swap(pending_transitions_);
    return out;
}

}  // namespace screener
