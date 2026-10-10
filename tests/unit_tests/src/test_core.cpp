#include <gtest/gtest.h>

#include <vector>

#include "md_core/core.h"

using namespace screener;

namespace {

constexpr Price P(double real) {
    return static_cast<Price>(real * static_cast<double>(kPriceScale));
}
constexpr Volume V(double real) {
    return static_cast<Volume>(real * static_cast<double>(kVolumeScale));
}

Candle Bar(uint32_t symbol_id, int64_t open_time_ms, double close, double turnover, double range_pct = 2.0) {
    const double half = close * range_pct / 200.0;
    Candle c{};
    c.symbol_id = symbol_id;
    c.open_time_ms = open_time_ms;
    c.event_time_ms = open_time_ms + kLtfMs;
    c.open = P(close);
    c.high = P(close + half);
    c.low = P(close - half);
    c.close = P(close);
    c.turnover = V(turnover);
    return c;
}

// Feeds `count` bars starting at an ALIGNED open time, so the aggregator never
// discards a leading partial group.
std::vector<Candle> History(uint32_t symbol_id, int count, double close, double turnover) {
    std::vector<Candle> out;
    for (int i = 0; i < count; ++i) {
        out.push_back(Bar(symbol_id, static_cast<int64_t>(i) * kLtfMs, close, turnover));
    }
    return out;
}

// Same shape as Bar(), spaced by kFastMs instead of kLtfMs. Used with
// WarmupFast()/ApplyFastCandle() - the independent 5m stream that now feeds
// surge, liquidity and trend's price leg (DESIGN.md §16).
Candle FastBar(uint32_t symbol_id, int64_t open_time_ms, double close, double turnover) {
    return Bar(symbol_id, open_time_ms, close, turnover);
}

// 2 x kFastSurgeWindowBars (576) 5m bars = the two 24h windows the fast
// tracker's surge/liquidity read. `recent_turnover` fills the newer half,
// `prev_turnover` the older half - same shape as the old LTF surge fixtures,
// just at 5m granularity and on the stream that now owns the window.
std::vector<Candle> FastHistory(uint32_t symbol_id, double close, double recent_turnover, double prev_turnover) {
    std::vector<Candle> out;
    constexpr int kBars = 2 * kFastSurgeWindowBars;
    for (int i = 0; i < kBars; ++i) {
        const double turnover = i >= kFastSurgeWindowBars ? recent_turnover : prev_turnover;
        out.push_back(FastBar(symbol_id, static_cast<int64_t>(i) * kFastMs, close, turnover));
    }
    return out;
}

// Thresholds matching what used to be compile-time constants, now that they
// are config.json fields (DESIGN.md §15) - keeps every existing test's
// fixtures and expectations unchanged except where a test explicitly varies
// a threshold.
CoreManager MakeCore() { return CoreManager(/*ema_period=*/50, /*surge_numerator=*/130, /*surge_denominator=*/100,
                                             /*min_natr_bp=*/100, /*min_turnover=*/0); }

}  // namespace

TEST(CoreManager, UniverseAssignsIdsAsIndices) {
    CoreManager core = MakeCore();
    core.SetUniverse({"BTCUSDT", "ETHUSDT", "SOLUSDT"});

    ASSERT_EQ(core.SymbolCount(), 3u);
    ASSERT_NE(core.FindId("BTCUSDT"), nullptr);
    EXPECT_EQ(*core.FindId("BTCUSDT"), 0u);
    EXPECT_EQ(*core.FindId("ETHUSDT"), 1u);
    EXPECT_EQ(*core.FindId("SOLUSDT"), 2u);
    EXPECT_EQ(core.FindId("NOPEUSDT"), nullptr);
    EXPECT_EQ(core.NameOf(1), "ETHUSDT");
}

// ---------------------------------------------------------------------------
// Readiness
// ---------------------------------------------------------------------------

// A symbol without enough history is `not ready` and can never be ACTIVE -
// never "probably fine".
TEST(CoreManager, NotReadyCanNeverBeActive) {
    CoreManager core = MakeCore();
    core.SetUniverse({"BTCUSDT"});

    // 40 bars: not enough for the 48-bar turnover window or the 50-bar EMA on
    // the 4h timeframe.
    const auto history = History(0, 40, 100.0, 1.0);
    core.Warmup(0, history);

    const auto conditions = core.EvaluateConditions(0);
    EXPECT_FALSE(conditions.ready);
    EXPECT_FALSE(conditions.All());
    EXPECT_TRUE(core.ActiveInstruments().empty());
}

TEST(CoreManager, BecomesReadyOnceEveryIndicatorHasHistory) {
    CoreManager core = MakeCore();
    core.SetUniverse({"BTCUSDT"});

    // EMA50 on 4h needs 50 HTF bars = 200 LTF bars.
    const auto history = History(0, 300, 100.0, 1.0);
    core.Warmup(0, history);
    // And the fast turnover window needs its own 576 bars of 5m history
    // (DESIGN.md §16) - readiness is now ANDed across three streams, not two.
    core.WarmupFast(0, FastHistory(0, 100.0, 1.0, 1.0));

    EXPECT_TRUE(core.EvaluateConditions(0).ready);
}

// Readiness is per STREAM. The 1h/4h side can be fully warm while the fast
// side has never been fed at all, and the symbol must still be not ready -
// this is the regression guard for EvaluateConditions ANDing in fast's
// Turnover().ready alongside ltf/htf readiness.
TEST(CoreManager, NotReadyWithoutTheFastStreamEvenIfLtfAndHtfAreWarm) {
    CoreManager core = MakeCore();
    core.SetUniverse({"BTCUSDT"});

    core.Warmup(0, History(0, 300, 100.0, 1.0));

    EXPECT_FALSE(core.EvaluateConditions(0).ready);
}

// ---------------------------------------------------------------------------
// Arrival classification: dedup, stale, gap
// ---------------------------------------------------------------------------

TEST(CoreManager, DuplicateOpenTimeIsDropped) {
    CoreManager core = MakeCore();
    core.SetUniverse({"BTCUSDT"});
    core.Warmup(0, History(0, 300, 100.0, 1.0));

    const int64_t next_open = 300 * kLtfMs;
    core.ApplyCandle(Bar(0, next_open, 110.0, 1.0));
    const uint32_t bars_after_first = core.TrackerAt(0).Ltf().Bars();

    // Same open_time again - a redelivery, or a replayed frame. The envelope
    // ts would differ; open_time is the identity, so this is the same bar.
    core.ApplyCandle(Bar(0, next_open, 999.0, 50.0));

    EXPECT_EQ(core.TrackerAt(0).Ltf().Bars(), bars_after_first);
    EXPECT_EQ(core.TrackerAt(0).Ltf().LastClose(), P(110.0));
}

TEST(CoreManager, OlderBarIsDropped) {
    CoreManager core = MakeCore();
    core.SetUniverse({"BTCUSDT"});
    core.Warmup(0, History(0, 300, 100.0, 1.0));

    core.ApplyCandle(Bar(0, 300 * kLtfMs, 110.0, 1.0));
    const uint32_t bars = core.TrackerAt(0).Ltf().Bars();

    core.ApplyCandle(Bar(0, 250 * kLtfMs, 1.0, 1.0));  // far in the past

    EXPECT_EQ(core.TrackerAt(0).Ltf().Bars(), bars);
    EXPECT_EQ(core.TrackerAt(0).Ltf().LastClose(), P(110.0));
}

TEST(CoreManager, ExactNextBarIsApplied) {
    CoreManager core = MakeCore();
    core.SetUniverse({"BTCUSDT"});
    core.Warmup(0, History(0, 300, 100.0, 1.0));

    const uint32_t before = core.TrackerAt(0).Ltf().Bars();
    core.ApplyCandle(Bar(0, 300 * kLtfMs, 110.0, 1.0));

    EXPECT_EQ(core.TrackerAt(0).Ltf().Bars(), before + 1);
    EXPECT_EQ(core.TrackerAt(0).LastLtfOpenTime(), 300 * kLtfMs);
}

// ---------------------------------------------------------------------------
// Gap handling
// ---------------------------------------------------------------------------

TEST(CoreManager, GapQueuesARebuildAndDoesNotApplyTheBar) {
    CoreManager core = MakeCore();
    core.SetUniverse({"BTCUSDT"});
    core.Warmup(0, History(0, 300, 100.0, 1.0));

    const uint32_t before = core.TrackerAt(0).Ltf().Bars();

    // Two hours missing.
    core.ApplyCandle(Bar(0, 302 * kLtfMs, 110.0, 1.0));

    EXPECT_EQ(core.TrackerAt(0).Ltf().Bars(), before) << "the gap bar must not be applied";
    EXPECT_TRUE(core.TrackerAt(0).RebuildPending());

    const auto pending = core.TakePendingRebuilds();
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending[0], 0u);
}

TEST(CoreManager, GapForcesTheSymbolInactiveImmediately) {
    CoreManager core = MakeCore();
    core.SetUniverse({"BTCUSDT"});

    // Build a state that passes every condition, then confirm it is ACTIVE
    // before the gap. Rising LTF closes keep ATR/EMA warm; the fast fixture
    // supplies the surge and the trend price leg (DESIGN.md §16) - a high
    // close that clears whatever EMA50 the rising LTF history produces.
    std::vector<Candle> history;
    for (int i = 0; i < 300; ++i) {
        history.push_back(Bar(0, static_cast<int64_t>(i) * kLtfMs, 100.0 + i, 1.0));
    }
    core.Warmup(0, history);
    core.WarmupFast(0, FastHistory(0, 1000.0, 100.0, 1.0));
    ASSERT_TRUE(core.TrackerAt(0).IsActive()) << "fixture must be ACTIVE before the gap";
    ASSERT_EQ(core.ActiveInstruments().count("BTCUSDT"), 1u);

    core.ApplyCandle(Bar(0, 310 * kLtfMs, 500.0, 500.0));

    // Fail safe: the indicators are known-wrong, so the symbol stops being
    // reported BEFORE anything is repaired.
    EXPECT_FALSE(core.TrackerAt(0).IsActive());
    EXPECT_EQ(core.ActiveInstruments().count("BTCUSDT"), 0u);
}

// The rebuild is DEFERRED, not inline. ApplyCandle only records the id; the
// main loop drains it. Servicing it inline would re-enter ApplyCandle once
// per history bar while the outer call is still on the stack.
TEST(CoreManager, PendingRebuildsAreDrainedNotServicedInline) {
    CoreManager core = MakeCore();
    core.SetUniverse({"BTCUSDT", "ETHUSDT"});
    core.Warmup(0, History(0, 300, 100.0, 1.0));
    core.Warmup(1, History(1, 300, 100.0, 1.0));

    core.ApplyCandle(Bar(0, 305 * kLtfMs, 100.0, 1.0));
    core.ApplyCandle(Bar(1, 307 * kLtfMs, 100.0, 1.0));

    const auto pending = core.TakePendingRebuilds();
    EXPECT_EQ(pending.size(), 2u);
    // And the list is now empty - a second drain must not repeat the work.
    EXPECT_TRUE(core.TakePendingRebuilds().empty());
}

TEST(CoreManager, ASecondGapDoesNotQueueTheSameSymbolTwice) {
    CoreManager core = MakeCore();
    core.SetUniverse({"BTCUSDT"});
    core.Warmup(0, History(0, 300, 100.0, 1.0));

    core.ApplyCandle(Bar(0, 305 * kLtfMs, 100.0, 1.0));
    core.ApplyCandle(Bar(0, 309 * kLtfMs, 100.0, 1.0));

    EXPECT_EQ(core.TakePendingRebuilds().size(), 1u);
}

// A bar closing during the blocking refetch arrives afterwards and is covered
// by the refetch itself - dedup drops it. This is what removed the need for a
// warm-up buffer entirely.
TEST(CoreManager, BarsArrivingBeforeTheRebuildAreDroppedAndRecoveredByIt) {
    CoreManager core = MakeCore();
    core.SetUniverse({"BTCUSDT"});
    core.Warmup(0, History(0, 300, 100.0, 1.0));

    core.ApplyCandle(Bar(0, 305 * kLtfMs, 100.0, 1.0));  // gap -> rebuild queued
    ASSERT_TRUE(core.TrackerAt(0).RebuildPending());

    const uint32_t bars = core.TrackerAt(0).Ltf().Bars();
    core.ApplyCandle(Bar(0, 306 * kLtfMs, 100.0, 1.0));
    EXPECT_EQ(core.TrackerAt(0).Ltf().Bars(), bars) << "bars during the stall must not be applied";

    // The rebuild replays history that already includes them.
    core.Warmup(0, History(0, 310, 100.0, 1.0));
    EXPECT_FALSE(core.TrackerAt(0).RebuildPending());
    EXPECT_EQ(core.TrackerAt(0).LastLtfOpenTime(), 309 * kLtfMs);
}

// ---------------------------------------------------------------------------
// Fast stream (5m) gap handling - DESIGN.md §16
// ---------------------------------------------------------------------------

TEST(CoreManager, FastGapQueuesAFastRebuildAndDoesNotApplyTheBar) {
    CoreManager core = MakeCore();
    core.SetUniverse({"BTCUSDT"});
    const auto fast_history = FastHistory(0, 100.0, 1.0, 1.0);
    core.WarmupFast(0, fast_history);

    const uint32_t before = core.TrackerAt(0).Fast().Bars();

    // One 5m period missing: the fixture's last bar opens at
    // (size-1)*kFastMs, so the next bar is expected at size*kFastMs.
    const int64_t gap_open_time = (static_cast<int64_t>(fast_history.size()) + 1) * kFastMs;
    core.ApplyFastCandle(FastBar(0, gap_open_time, 110.0, 1.0));

    EXPECT_EQ(core.TrackerAt(0).Fast().Bars(), 0u) << "ResetFast() fails closed immediately, not just on the next bar";
    EXPECT_NE(before, 0u) << "sanity: the fixture really had bars before the gap";
    EXPECT_TRUE(core.TrackerAt(0).FastRebuildPending());

    const auto pending = core.TakePendingFastRebuilds();
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending[0], 0u);
    // The 1h rebuild list is untouched - a 5m gap is not a 1h gap.
    EXPECT_TRUE(core.TakePendingRebuilds().empty());
}

TEST(CoreManager, ASecondFastGapDoesNotQueueTheSameSymbolTwice) {
    CoreManager core = MakeCore();
    core.SetUniverse({"BTCUSDT"});
    core.WarmupFast(0, FastHistory(0, 100.0, 1.0, 1.0));

    core.ApplyFastCandle(FastBar(0, 600 * kFastMs, 100.0, 1.0));
    core.ApplyFastCandle(FastBar(0, 604 * kFastMs, 100.0, 1.0));

    EXPECT_EQ(core.TakePendingFastRebuilds().size(), 1u);
}

// The core of DESIGN.md §16's "lighter rebuild": a 5m gap must NOT touch the
// 1h/4h trackers, and a 1h gap in flight at the same time must not be
// affected by it either - the two rebuild paths are fully independent.
TEST(CoreManager, FastGapDoesNotTouchTheOneHourOrFourHourTrackers) {
    CoreManager core = MakeCore();
    core.SetUniverse({"BTCUSDT"});
    core.Warmup(0, History(0, 300, 100.0, 1.0));
    core.WarmupFast(0, FastHistory(0, 100.0, 1.0, 1.0));

    const uint32_t ltf_bars = core.TrackerAt(0).Ltf().Bars();
    const uint32_t htf_bars = core.TrackerAt(0).Htf().Bars();
    const Price ema = core.TrackerAt(0).Htf().GetEma();

    core.ApplyFastCandle(FastBar(0, 600 * kFastMs, 110.0, 1.0));  // 5m gap

    EXPECT_EQ(core.TrackerAt(0).Ltf().Bars(), ltf_bars);
    EXPECT_EQ(core.TrackerAt(0).Htf().Bars(), htf_bars);
    EXPECT_EQ(core.TrackerAt(0).Htf().GetEma(), ema);
    // And a live 1h bar still applies normally while the fast rebuild is
    // pending - the two streams really are independent, not just "don't
    // crash".
    core.ApplyCandle(Bar(0, 300 * kLtfMs, 110.0, 1.0));
    EXPECT_EQ(core.TrackerAt(0).Ltf().Bars(), ltf_bars + 1);
}

// WarmupFast() must reset and replay ONLY the fast tracker - a regression
// guard for the same reason Warmup()/Reset() stay separate from it.
TEST(CoreManager, WarmupFastDoesNotResetLtfOrHtf) {
    CoreManager core = MakeCore();
    core.SetUniverse({"BTCUSDT"});
    core.Warmup(0, History(0, 300, 100.0, 1.0));

    const uint32_t ltf_bars = core.TrackerAt(0).Ltf().Bars();
    const uint32_t htf_bars = core.TrackerAt(0).Htf().Bars();

    core.WarmupFast(0, FastHistory(0, 100.0, 1.0, 1.0));

    EXPECT_EQ(core.TrackerAt(0).Ltf().Bars(), ltf_bars);
    EXPECT_EQ(core.TrackerAt(0).Htf().Bars(), htf_bars);
}

// ---------------------------------------------------------------------------
// Apply-before-evaluate ordering
// ---------------------------------------------------------------------------

// The bar that closes a 4h candle must update the HTF EMA BEFORE the filter
// runs. Otherwise, on one bar in four, the trend test compares a fresh 1h
// close against a one-period-stale EMA50 - wrong once every four hours, which
// is rare enough to survive casual testing and frequent enough to matter.
TEST(CoreManager, HtfIsUpdatedBeforeTheFilterRunsOnABoundaryBar) {
    CoreManager core = MakeCore();
    core.SetUniverse({"BTCUSDT"});
    core.Warmup(0, History(0, 300, 100.0, 1.0));

    const uint32_t htf_bars_before = core.TrackerAt(0).Htf().Bars();
    const Price ema_before = core.TrackerAt(0).Htf().GetEma();

    // 300 * 1h is a 4h boundary start, so bar 303 closes the group.
    core.ApplyCandle(Bar(0, 300 * kLtfMs, 200.0, 1.0));
    core.ApplyCandle(Bar(0, 301 * kLtfMs, 200.0, 1.0));
    core.ApplyCandle(Bar(0, 302 * kLtfMs, 200.0, 1.0));
    EXPECT_EQ(core.TrackerAt(0).Htf().Bars(), htf_bars_before) << "no HTF bar yet";

    core.ApplyCandle(Bar(0, 303 * kLtfMs, 200.0, 1.0));

    EXPECT_EQ(core.TrackerAt(0).Htf().Bars(), htf_bars_before + 1);
    EXPECT_NE(core.TrackerAt(0).Htf().GetEma(), ema_before) << "EMA must already reflect the new HTF bar";
}

// ---------------------------------------------------------------------------
// Transitions
// ---------------------------------------------------------------------------

TEST(CoreManager, ActivatesWhenAllThreeConditionsHold) {
    CoreManager core = MakeCore();
    core.SetUniverse({"BTCUSDT"});

    std::vector<Candle> history;
    for (int i = 0; i < 300; ++i) {
        history.push_back(Bar(0, static_cast<int64_t>(i) * kLtfMs, 100.0 + i, 1.0));
    }
    core.Warmup(0, history);
    // Surge and trend's price leg now come from the fast (5m) stream
    // (DESIGN.md §16): a high close clears whatever EMA50 the rising LTF
    // history produces, and the recent half of the window carries far more
    // turnover than the earlier half.
    core.WarmupFast(0, FastHistory(0, 1000.0, 100.0, 1.0));

    const auto c = core.EvaluateConditions(0);
    EXPECT_TRUE(c.ready);
    EXPECT_TRUE(c.surge);
    EXPECT_TRUE(c.trend);
    EXPECT_TRUE(c.volatility);
    EXPECT_TRUE(core.TrackerAt(0).IsActive());
    EXPECT_EQ(core.ActiveInstruments().count("BTCUSDT"), 1u);
}

TEST(CoreManager, FlatTurnoverFailsTheSurgeCondition) {
    CoreManager core = MakeCore();
    core.SetUniverse({"BTCUSDT"});

    core.Warmup(0, History(0, 300, 100.0, 1.0));
    // Flat turnover on the fast stream: recent == prev, so the surge ratio
    // never clears 1.30x.
    core.WarmupFast(0, FastHistory(0, 1000.0, 1.0, 1.0));

    const auto c = core.EvaluateConditions(0);
    EXPECT_TRUE(c.ready);
    EXPECT_FALSE(c.surge);
    EXPECT_FALSE(core.TrackerAt(0).IsActive());
}

// Exactly +30% must NOT pass: the test is a strict inequality, so this pins
// the boundary rather than leaving it to a later refactor to guess.
TEST(CoreManager, SurgeBoundaryIsExclusive) {
    CoreManager core = MakeCore();
    core.SetUniverse({"BTCUSDT"});

    core.Warmup(0, History(0, 300, 100.0, 1.0));
    core.WarmupFast(0, FastHistory(0, 1000.0, 1.3, 1.0));

    EXPECT_FALSE(core.EvaluateConditions(0).surge);
}

TEST(CoreManager, LowVolatilityFailsTheNatrCondition) {
    CoreManager core = MakeCore();
    core.SetUniverse({"BTCUSDT"});

    std::vector<Candle> history;
    for (int i = 0; i < 300; ++i) {
        // 0.1% range: NATR well under the 100bp floor. Volatility is still
        // read from the LTF's own ATR (DESIGN.md §16 leaves it untouched),
        // so only this fixture's range matters here.
        history.push_back(Bar(0, static_cast<int64_t>(i) * kLtfMs, 100.0 + i, 1.0, 0.1));
    }
    core.Warmup(0, history);
    core.WarmupFast(0, FastHistory(0, 1000.0, 100.0, 1.0));

    const auto c = core.EvaluateConditions(0);
    EXPECT_TRUE(c.surge);
    EXPECT_FALSE(c.volatility);
    EXPECT_FALSE(core.TrackerAt(0).IsActive());
}

// min_turnover = 0 (MakeCore's default) must behave exactly like the
// pre-config.json compile-time constants - this is the regression guard for
// the new liquidity condition.
TEST(CoreManager, LiquidityIsSatisfiedByDefault) {
    CoreManager core = MakeCore();
    core.SetUniverse({"BTCUSDT"});

    core.Warmup(0, History(0, 300, 100.0, 1.0));
    core.WarmupFast(0, FastHistory(0, 1000.0, 100.0, 1.0));

    EXPECT_TRUE(core.EvaluateConditions(0).liquidity);
}

// A min_turnover above the fixture's recent-window sum must fail the
// liquidity condition and keep the symbol out of ACTIVE even though
// surge/trend/volatility all hold - same fixture as
// ActivatesWhenAllThreeConditionsHold, only the threshold differs.
//
// recent-window sum = kFastSurgeWindowBars (288) * 100.0, at kVolumeScale.
TEST(CoreManager, TurnoverBelowTheFloorFailsTheLiquidityCondition) {
    CoreManager core(/*ema_period=*/50, /*surge_numerator=*/130, /*surge_denominator=*/100,
                      /*min_natr_bp=*/100, /*min_turnover=*/V(100.0) * kFastSurgeWindowBars + 1);
    core.SetUniverse({"BTCUSDT"});

    core.Warmup(0, History(0, 300, 100.0, 1.0));
    core.WarmupFast(0, FastHistory(0, 1000.0, 100.0, 1.0));

    const auto c = core.EvaluateConditions(0);
    EXPECT_TRUE(c.surge);
    EXPECT_TRUE(c.trend);
    EXPECT_TRUE(c.volatility);
    EXPECT_FALSE(c.liquidity);
    EXPECT_FALSE(core.TrackerAt(0).IsActive());
}

// ema_period is a config.json field now (DESIGN.md §15), not the compiled
// constant 50 - this proves warm-up and the trend condition both honour a
// different period, not a hardcoded one.
TEST(CoreManager, HonoursARuntimeEmaPeriodOtherThanFifty) {
    CoreManager core(/*ema_period=*/10, /*surge_numerator=*/130, /*surge_denominator=*/100,
                      /*min_natr_bp=*/100, /*min_turnover=*/0);
    core.SetUniverse({"BTCUSDT"});

    // Period 10 on the HTF (4h) needs 10 * 4 = 40 LTF bars to produce a first
    // EMA value - far fewer than period 50's 200, so a short history that
    // would leave the default-period fixture `not ready` is enough here.
    std::vector<Candle> history;
    for (int i = 0; i < 80; ++i) {
        history.push_back(Bar(0, static_cast<int64_t>(i) * kLtfMs, 100.0 + i, 1.0));
    }
    core.Warmup(0, history);
    core.WarmupFast(0, FastHistory(0, 1000.0, 100.0, 1.0));

    const auto c = core.EvaluateConditions(0);
    EXPECT_TRUE(c.ready);
    EXPECT_TRUE(c.trend);
    EXPECT_TRUE(core.TrackerAt(0).IsActive());
}

TEST(CoreManager, DeactivatesWhenAConditionStopsHolding) {
    CoreManager core = MakeCore();
    core.SetUniverse({"BTCUSDT"});

    core.Warmup(0, History(0, 300, 100.0, 1.0));
    const auto fast_history = FastHistory(0, 1000.0, 100.0, 1.0);
    core.WarmupFast(0, fast_history);
    ASSERT_TRUE(core.TrackerAt(0).IsActive());

    // Trend now reads the FAST stream's last close (DESIGN.md §16), so a
    // collapse has to arrive on THAT stream, continuing it exactly where
    // WarmupFast left off, to drop below EMA50 and kill the trend condition.
    core.ApplyFastCandle(FastBar(0, static_cast<int64_t>(fast_history.size()) * kFastMs, 1.0, 100.0));

    EXPECT_FALSE(core.TrackerAt(0).IsActive());
    EXPECT_EQ(core.ActiveInstruments().count("BTCUSDT"), 0u);
}

// ---------------------------------------------------------------------------
// Transitions - TakeTransitions()
// ---------------------------------------------------------------------------

// Warmup's internal Evaluate() call activates the symbol directly, so the
// transition is already pending the moment Warmup returns - no ApplyCandle
// needed to observe it.
TEST(CoreManager, ActivatingPushesATransition) {
    CoreManager core = MakeCore();
    core.SetUniverse({"BTCUSDT"});

    core.Warmup(0, History(0, 300, 100.0, 1.0));
    core.WarmupFast(0, FastHistory(0, 1000.0, 100.0, 1.0));
    ASSERT_TRUE(core.TrackerAt(0).IsActive());

    const auto transitions = core.TakeTransitions();
    ASSERT_EQ(transitions.size(), 1u);
    EXPECT_EQ(transitions[0].symbol_id, 0u);
    EXPECT_TRUE(transitions[0].active);
}

TEST(CoreManager, DeactivatingPushesATransition) {
    CoreManager core = MakeCore();
    core.SetUniverse({"BTCUSDT"});

    core.Warmup(0, History(0, 300, 100.0, 1.0));
    const auto fast_history = FastHistory(0, 1000.0, 100.0, 1.0);
    core.WarmupFast(0, fast_history);
    ASSERT_TRUE(core.TrackerAt(0).IsActive());
    core.TakeTransitions();  // drain the activation from Warmup

    // Collapse on the fast stream (DESIGN.md §16: trend now reads it) drops
    // below EMA50 and kills the trend condition.
    core.ApplyFastCandle(FastBar(0, static_cast<int64_t>(fast_history.size()) * kFastMs, 1.0, 100.0));
    ASSERT_FALSE(core.TrackerAt(0).IsActive());

    const auto transitions = core.TakeTransitions();
    ASSERT_EQ(transitions.size(), 1u);
    EXPECT_EQ(transitions[0].symbol_id, 0u);
    EXPECT_FALSE(transitions[0].active);
}

TEST(CoreManager, TakeTransitionsDrainsNotServicedInline) {
    CoreManager core = MakeCore();
    core.SetUniverse({"BTCUSDT"});

    core.Warmup(0, History(0, 300, 100.0, 1.0));
    core.WarmupFast(0, FastHistory(0, 1000.0, 100.0, 1.0));

    EXPECT_EQ(core.TakeTransitions().size(), 1u);
    // And the list is now empty - a second drain must not repeat the work.
    EXPECT_TRUE(core.TakeTransitions().empty());
}

// Edge-triggered: a bar that keeps the symbol ACTIVE must not add a second
// transition on top of the one Warmup already recorded.
TEST(CoreManager, StayingActiveDoesNotPushATransition) {
    CoreManager core = MakeCore();
    core.SetUniverse({"BTCUSDT"});

    core.Warmup(0, History(0, 300, 100.0, 1.0));
    const auto fast_history = FastHistory(0, 1000.0, 100.0, 1.0);
    core.WarmupFast(0, fast_history);
    ASSERT_TRUE(core.TrackerAt(0).IsActive());
    core.TakeTransitions();  // drain the activation from Warmup

    // Next fast bar keeps every condition holding: still a high close,
    // still surging.
    core.ApplyFastCandle(FastBar(0, static_cast<int64_t>(fast_history.size()) * kFastMs, 1000.0, 100.0));
    ASSERT_TRUE(core.TrackerAt(0).IsActive());

    EXPECT_TRUE(core.TakeTransitions().empty());
}

// RequestRebuild force-deactivates inline and bypasses TryDeactivate, so it
// must record the edge itself - miss it and chat keeps showing a symbol
// ACTIVE after a gap has already invalidated it.
TEST(CoreManager, GapForcesInactiveAndPushesATransition) {
    CoreManager core = MakeCore();
    core.SetUniverse({"BTCUSDT"});

    core.Warmup(0, History(0, 300, 100.0, 1.0));
    core.WarmupFast(0, FastHistory(0, 1000.0, 100.0, 1.0));
    ASSERT_TRUE(core.TrackerAt(0).IsActive());
    core.TakeTransitions();  // drain the activation from Warmup

    core.ApplyCandle(Bar(0, 310 * kLtfMs, 500.0, 500.0));  // gap, on the 1h stream
    ASSERT_FALSE(core.TrackerAt(0).IsActive());

    const auto transitions = core.TakeTransitions();
    ASSERT_EQ(transitions.size(), 1u);
    EXPECT_EQ(transitions[0].symbol_id, 0u);
    EXPECT_FALSE(transitions[0].active);
}

// A gap on the FAST stream must also force INACTIVE and record the edge,
// even though it bypasses RequestRebuild entirely - RequestFastRebuild fails
// closed through the normal Evaluate()/TryDeactivate() path instead
// (DESIGN.md §16), and that path must still produce a transition.
TEST(CoreManager, FastGapForcesInactiveAndPushesATransition) {
    CoreManager core = MakeCore();
    core.SetUniverse({"BTCUSDT"});

    core.Warmup(0, History(0, 300, 100.0, 1.0));
    const auto fast_history = FastHistory(0, 1000.0, 100.0, 1.0);
    core.WarmupFast(0, fast_history);
    ASSERT_TRUE(core.TrackerAt(0).IsActive());
    core.TakeTransitions();  // drain the activation from Warmup

    // One 5m period missing.
    const int64_t gap_open_time = (static_cast<int64_t>(fast_history.size()) + 1) * kFastMs;
    core.ApplyFastCandle(FastBar(0, gap_open_time, 1000.0, 100.0));
    ASSERT_FALSE(core.TrackerAt(0).IsActive());
    ASSERT_TRUE(core.TrackerAt(0).FastRebuildPending());

    const auto transitions = core.TakeTransitions();
    ASSERT_EQ(transitions.size(), 1u);
    EXPECT_EQ(transitions[0].symbol_id, 0u);
    EXPECT_FALSE(transitions[0].active);
}

// A gap on a symbol that was never ACTIVE has nothing to announce.
TEST(CoreManager, GapOnAnAlreadyInactiveSymbolPushesNoTransition) {
    CoreManager core = MakeCore();
    core.SetUniverse({"BTCUSDT"});
    core.Warmup(0, History(0, 300, 100.0, 1.0));
    core.TakeTransitions();  // drain anything Warmup might have recorded

    core.ApplyCandle(Bar(0, 305 * kLtfMs, 100.0, 1.0));  // gap
    ASSERT_TRUE(core.TrackerAt(0).RebuildPending());

    EXPECT_TRUE(core.TakeTransitions().empty());
}

TEST(CoreManager, SetUniverseClearsPendingTransitions) {
    CoreManager core = MakeCore();
    core.SetUniverse({"BTCUSDT"});

    core.Warmup(0, History(0, 300, 100.0, 1.0));
    core.WarmupFast(0, FastHistory(0, 1000.0, 100.0, 1.0));
    ASSERT_TRUE(core.TrackerAt(0).IsActive());  // transition pending, not yet drained

    core.SetUniverse({"BTCUSDT"});

    EXPECT_TRUE(core.TakeTransitions().empty());
}

// ---------------------------------------------------------------------------
// Determinism
// ---------------------------------------------------------------------------

TEST(CoreManager, ReplayIsDeterministic) {
    const auto history = History(0, 300, 100.0, 1.0);
    const auto fast_history = FastHistory(0, 1000.0, 100.0, 1.0);

    CoreManager a = MakeCore();
    CoreManager b = MakeCore();
    a.SetUniverse({"BTCUSDT"});
    b.SetUniverse({"BTCUSDT"});
    a.Warmup(0, history);
    b.Warmup(0, history);
    a.WarmupFast(0, fast_history);
    b.WarmupFast(0, fast_history);

    EXPECT_EQ(a.TrackerAt(0).Htf().GetEma(), b.TrackerAt(0).Htf().GetEma());
    EXPECT_EQ(a.TrackerAt(0).Ltf().GetNatrBp(), b.TrackerAt(0).Ltf().GetNatrBp());
    EXPECT_EQ(a.TrackerAt(0).Fast().Turnover().recent, b.TrackerAt(0).Fast().Turnover().recent);
}

TEST(CoreManager, UnknownSymbolIdIsIgnored) {
    CoreManager core = MakeCore();
    core.SetUniverse({"BTCUSDT"});
    // Must not crash or write out of bounds.
    core.ApplyCandle(Bar(99, kLtfMs, 100.0, 1.0));
    EXPECT_EQ(core.TrackerAt(0).Ltf().Bars(), 0u);
}
