#include <gtest/gtest.h>

#include "md_core/htf_aggregator.h"

using namespace screener;

namespace {

constexpr Price P(double real) {
    return static_cast<Price>(real * static_cast<double>(kPriceScale));
}
constexpr Volume V(double real) {
    return static_cast<Volume>(real * static_cast<double>(kVolumeScale));
}

Candle Bar(int64_t open_time_ms, double open, double high, double low, double close, double turnover) {
    Candle c{};
    c.open_time_ms = open_time_ms;
    c.event_time_ms = open_time_ms + kLtfMs;  // closed at the end of its hour
    c.open = P(open);
    c.high = P(high);
    c.low = P(low);
    c.close = P(close);
    c.turnover = V(turnover);
    c.symbol_id = 7;
    return c;
}

// A 4h boundary: 1970-01-01T00:00Z is one, so any multiple of 4h is.
constexpr int64_t kAligned = 0;

}  // namespace

TEST(HtfAggregator, BucketStartRoundsDownToFourHours) {
    EXPECT_EQ(HtfBucketStart(0), 0);
    EXPECT_EQ(HtfBucketStart(kLtfMs), 0);
    EXPECT_EQ(HtfBucketStart(3 * kLtfMs), 0);
    EXPECT_EQ(HtfBucketStart(4 * kLtfMs), 4 * kLtfMs);
    EXPECT_EQ(HtfBucketStart(5 * kLtfMs), 4 * kLtfMs);
}

TEST(HtfAggregator, EmitsOnTheFourthBarOnly) {
    HtfAggregator agg;
    EXPECT_FALSE(agg.Add(Bar(kAligned + 0 * kLtfMs, 10, 11, 9, 10, 1)).has_value());
    EXPECT_FALSE(agg.Add(Bar(kAligned + 1 * kLtfMs, 10, 12, 8, 11, 1)).has_value());
    EXPECT_FALSE(agg.Add(Bar(kAligned + 2 * kLtfMs, 11, 13, 10, 12, 1)).has_value());
    EXPECT_TRUE(agg.Add(Bar(kAligned + 3 * kLtfMs, 12, 14, 11, 13, 1)).has_value());
}

TEST(HtfAggregator, CombinesOhlcAndTurnoverCorrectly) {
    HtfAggregator agg;
    agg.Add(Bar(kAligned + 0 * kLtfMs, 10, 11, 9, 10, 1.0));
    agg.Add(Bar(kAligned + 1 * kLtfMs, 10, 20, 8, 11, 2.0));
    agg.Add(Bar(kAligned + 2 * kLtfMs, 11, 13, 3, 12, 4.0));
    const auto htf = agg.Add(Bar(kAligned + 3 * kLtfMs, 12, 14, 11, 99, 8.0));

    ASSERT_TRUE(htf.has_value());
    EXPECT_EQ(htf->open, P(10));   // FIRST bar's open
    EXPECT_EQ(htf->high, P(20));   // max across the group
    EXPECT_EQ(htf->low, P(3));     // min across the group
    EXPECT_EQ(htf->close, P(99));  // LAST bar's close
    EXPECT_EQ(htf->turnover, V(15.0));
    EXPECT_EQ(htf->symbol_id, 7u);
}

TEST(HtfAggregator, OpenTimeIsTheBucketStartNotTheFirstBar) {
    HtfAggregator agg;
    for (int i = 0; i < 3; ++i) {
        agg.Add(Bar(kAligned + i * kLtfMs, 10, 11, 9, 10, 1));
    }
    const auto htf = agg.Add(Bar(kAligned + 3 * kLtfMs, 10, 11, 9, 10, 1));
    ASSERT_TRUE(htf.has_value());
    EXPECT_EQ(htf->open_time_ms, kAligned);
}

// open_time identifies the BAR; event_time describes the MESSAGE. An HTF bar
// closes when its last LTF bar closes, so it inherits that message's ts while
// keeping the bucket start as its identity.
TEST(HtfAggregator, EventTimeComesFromTheBarThatClosedTheGroup) {
    HtfAggregator agg;
    for (int i = 0; i < 3; ++i) {
        agg.Add(Bar(kAligned + i * kLtfMs, 10, 11, 9, 10, 1));
    }
    const Candle last = Bar(kAligned + 3 * kLtfMs, 10, 11, 9, 10, 1);
    const auto htf = agg.Add(last);
    ASSERT_TRUE(htf.has_value());
    EXPECT_EQ(htf->event_time_ms, last.event_time_ms);
}

// THE ALIGNMENT TRAP.
//
// Warm-up hands over an arbitrary window of history, so its oldest bar is
// usually mid-bucket. Aggregating it anyway would build a "4h" candle from
// 1-3 bars, and that wrong bar seeds the EMA50 - an error that decays over
// ~50 bars, so it is largest where nobody looks and invisible by the time
// anybody does.
TEST(HtfAggregator, DiscardsTheLeadingPartialGroup) {
    HtfAggregator agg;

    // Start one hour INTO a bucket: these three must produce nothing.
    EXPECT_FALSE(agg.Add(Bar(kAligned + 1 * kLtfMs, 10, 11, 9, 10, 1)).has_value());
    EXPECT_FALSE(agg.Add(Bar(kAligned + 2 * kLtfMs, 10, 11, 9, 10, 1)).has_value());
    EXPECT_FALSE(agg.Add(Bar(kAligned + 3 * kLtfMs, 10, 11, 9, 10, 1)).has_value());

    // The next bucket starts cleanly and completes normally.
    const int64_t next = kAligned + 4 * kLtfMs;
    EXPECT_FALSE(agg.Add(Bar(next + 0 * kLtfMs, 10, 11, 9, 10, 1)).has_value());
    EXPECT_FALSE(agg.Add(Bar(next + 1 * kLtfMs, 10, 11, 9, 10, 1)).has_value());
    EXPECT_FALSE(agg.Add(Bar(next + 2 * kLtfMs, 10, 11, 9, 10, 1)).has_value());
    const auto htf = agg.Add(Bar(next + 3 * kLtfMs, 10, 11, 9, 10, 1));
    ASSERT_TRUE(htf.has_value());
    EXPECT_EQ(htf->open_time_ms, next);
}

// The same history trimmed to a boundary must give the same result as the
// untrimmed version - that equivalence is what makes `limit=603` safe instead
// of merely generous.
TEST(HtfAggregator, UnalignedAndTrimmedHistoryAgree) {
    std::vector<Candle> unaligned;
    for (int i = 1; i < 13; ++i) {  // starts at +1h: 11 bars, 2 full buckets
        unaligned.push_back(Bar(kAligned + i * kLtfMs, 10 + i, 11 + i, 9 + i, 10 + i, 1));
    }
    std::vector<Candle> trimmed(unaligned.begin() + 3, unaligned.end());

    std::vector<Candle> from_unaligned;
    std::vector<Candle> from_trimmed;
    HtfAggregator a;
    HtfAggregator b;
    for (const Candle& c : unaligned) {
        if (auto h = a.Add(c)) {
            from_unaligned.push_back(*h);
        }
    }
    for (const Candle& c : trimmed) {
        if (auto h = b.Add(c)) {
            from_trimmed.push_back(*h);
        }
    }

    ASSERT_EQ(from_unaligned.size(), from_trimmed.size());
    for (std::size_t i = 0; i < from_unaligned.size(); ++i) {
        EXPECT_EQ(from_unaligned[i].open_time_ms, from_trimmed[i].open_time_ms);
        EXPECT_EQ(from_unaligned[i].open, from_trimmed[i].open);
        EXPECT_EQ(from_unaligned[i].high, from_trimmed[i].high);
        EXPECT_EQ(from_unaligned[i].low, from_trimmed[i].low);
        EXPECT_EQ(from_unaligned[i].close, from_trimmed[i].close);
        EXPECT_EQ(from_unaligned[i].turnover, from_trimmed[i].turnover);
    }
}

TEST(HtfAggregator, ResetDropsThePartialBucket) {
    HtfAggregator agg;
    agg.Add(Bar(kAligned + 0 * kLtfMs, 10, 11, 9, 10, 1));
    agg.Add(Bar(kAligned + 1 * kLtfMs, 10, 11, 9, 10, 1));
    EXPECT_EQ(agg.Pending(), 2);
    agg.Reset();
    EXPECT_EQ(agg.Pending(), 0);
}
