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

}  // namespace

TEST(CoreManager, UniverseAssignsIdsAsIndices) {
    CoreManager core;
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
    CoreManager core;
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
    CoreManager core;
    core.SetUniverse({"BTCUSDT"});

    // EMA50 on 4h needs 50 HTF bars = 200 LTF bars.
    const auto history = History(0, 300, 100.0, 1.0);
    core.Warmup(0, history);

    EXPECT_TRUE(core.EvaluateConditions(0).ready);
}

// ---------------------------------------------------------------------------
// Arrival classification: dedup, stale, gap
// ---------------------------------------------------------------------------

TEST(CoreManager, DuplicateOpenTimeIsDropped) {
    CoreManager core;
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
    CoreManager core;
    core.SetUniverse({"BTCUSDT"});
    core.Warmup(0, History(0, 300, 100.0, 1.0));

    core.ApplyCandle(Bar(0, 300 * kLtfMs, 110.0, 1.0));
    const uint32_t bars = core.TrackerAt(0).Ltf().Bars();

    core.ApplyCandle(Bar(0, 250 * kLtfMs, 1.0, 1.0));  // far in the past

    EXPECT_EQ(core.TrackerAt(0).Ltf().Bars(), bars);
    EXPECT_EQ(core.TrackerAt(0).Ltf().LastClose(), P(110.0));
}

TEST(CoreManager, ExactNextBarIsApplied) {
    CoreManager core;
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
    CoreManager core;
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
    CoreManager core;
    core.SetUniverse({"BTCUSDT"});

    // Build a state that passes all three conditions, then confirm it is
    // ACTIVE before the gap.
    std::vector<Candle> history;
    for (int i = 0; i < 300; ++i) {
        // Rising closes keep close > EMA50; the last 24 bars carry much more
        // turnover than the 24 before them.
        const double close = 100.0 + i;
        const double turnover = i >= 276 ? 100.0 : 1.0;
        history.push_back(Bar(0, static_cast<int64_t>(i) * kLtfMs, close, turnover));
    }
    core.Warmup(0, history);
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
    CoreManager core;
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
    CoreManager core;
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
    CoreManager core;
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
// Apply-before-evaluate ordering
// ---------------------------------------------------------------------------

// The bar that closes a 4h candle must update the HTF EMA BEFORE the filter
// runs. Otherwise, on one bar in four, the trend test compares a fresh 1h
// close against a one-period-stale EMA50 - wrong once every four hours, which
// is rare enough to survive casual testing and frequent enough to matter.
TEST(CoreManager, HtfIsUpdatedBeforeTheFilterRunsOnABoundaryBar) {
    CoreManager core;
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
    CoreManager core;
    core.SetUniverse({"BTCUSDT"});

    std::vector<Candle> history;
    for (int i = 0; i < 300; ++i) {
        const double close = 100.0 + i;                  // rising -> close > EMA50
        const double turnover = i >= 276 ? 100.0 : 1.0;  // last 24 bars surge
        history.push_back(Bar(0, static_cast<int64_t>(i) * kLtfMs, close, turnover));
    }
    core.Warmup(0, history);

    const auto c = core.EvaluateConditions(0);
    EXPECT_TRUE(c.ready);
    EXPECT_TRUE(c.surge);
    EXPECT_TRUE(c.trend);
    EXPECT_TRUE(c.volatility);
    EXPECT_TRUE(core.TrackerAt(0).IsActive());
    EXPECT_EQ(core.ActiveInstruments().count("BTCUSDT"), 1u);
}

TEST(CoreManager, FlatTurnoverFailsTheSurgeCondition) {
    CoreManager core;
    core.SetUniverse({"BTCUSDT"});

    std::vector<Candle> history;
    for (int i = 0; i < 300; ++i) {
        history.push_back(Bar(0, static_cast<int64_t>(i) * kLtfMs, 100.0 + i, 1.0));
    }
    core.Warmup(0, history);

    const auto c = core.EvaluateConditions(0);
    EXPECT_TRUE(c.ready);
    EXPECT_FALSE(c.surge);
    EXPECT_FALSE(core.TrackerAt(0).IsActive());
}

// Exactly +30% must NOT pass: the test is a strict inequality, so this pins
// the boundary rather than leaving it to a later refactor to guess.
TEST(CoreManager, SurgeBoundaryIsExclusive) {
    CoreManager core;
    core.SetUniverse({"BTCUSDT"});

    std::vector<Candle> history;
    for (int i = 0; i < 300; ++i) {
        const double turnover = i >= 276 ? 1.3 : 1.0;
        history.push_back(Bar(0, static_cast<int64_t>(i) * kLtfMs, 100.0 + i, turnover));
    }
    core.Warmup(0, history);

    EXPECT_FALSE(core.EvaluateConditions(0).surge);
}

TEST(CoreManager, LowVolatilityFailsTheNatrCondition) {
    CoreManager core;
    core.SetUniverse({"BTCUSDT"});

    std::vector<Candle> history;
    for (int i = 0; i < 300; ++i) {
        const double turnover = i >= 276 ? 100.0 : 1.0;
        // 0.1% range: NATR well under the 100bp floor.
        history.push_back(Bar(0, static_cast<int64_t>(i) * kLtfMs, 100.0 + i, turnover, 0.1));
    }
    core.Warmup(0, history);

    const auto c = core.EvaluateConditions(0);
    EXPECT_TRUE(c.surge);
    EXPECT_FALSE(c.volatility);
    EXPECT_FALSE(core.TrackerAt(0).IsActive());
}

TEST(CoreManager, DeactivatesWhenAConditionStopsHolding) {
    CoreManager core;
    core.SetUniverse({"BTCUSDT"});

    std::vector<Candle> history;
    for (int i = 0; i < 300; ++i) {
        const double turnover = i >= 276 ? 100.0 : 1.0;
        history.push_back(Bar(0, static_cast<int64_t>(i) * kLtfMs, 100.0 + i, turnover));
    }
    core.Warmup(0, history);
    ASSERT_TRUE(core.TrackerAt(0).IsActive());

    // A collapse in close drops it below the EMA50 and kills the trend
    // condition.
    core.ApplyCandle(Bar(0, 300 * kLtfMs, 1.0, 100.0));

    EXPECT_FALSE(core.TrackerAt(0).IsActive());
    EXPECT_EQ(core.ActiveInstruments().count("BTCUSDT"), 0u);
}

// ---------------------------------------------------------------------------
// Determinism
// ---------------------------------------------------------------------------

TEST(CoreManager, ReplayIsDeterministic) {
    const auto history = History(0, 300, 100.0, 1.0);

    CoreManager a;
    CoreManager b;
    a.SetUniverse({"BTCUSDT"});
    b.SetUniverse({"BTCUSDT"});
    a.Warmup(0, history);
    b.Warmup(0, history);

    EXPECT_EQ(a.TrackerAt(0).Htf().GetEma(), b.TrackerAt(0).Htf().GetEma());
    EXPECT_EQ(a.TrackerAt(0).Ltf().GetNatrBp(), b.TrackerAt(0).Ltf().GetNatrBp());
    EXPECT_EQ(a.TrackerAt(0).Ltf().Turnover().recent, b.TrackerAt(0).Ltf().Turnover().recent);
}

TEST(CoreManager, UnknownSymbolIdIsIgnored) {
    CoreManager core;
    core.SetUniverse({"BTCUSDT"});
    // Must not crash or write out of bounds.
    core.ApplyCandle(Bar(99, kLtfMs, 100.0, 1.0));
    EXPECT_EQ(core.TrackerAt(0).Ltf().Bars(), 0u);
}
