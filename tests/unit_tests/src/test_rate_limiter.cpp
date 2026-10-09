// RateLimiter tests.
//
// The async path exists because the warm-up now overlaps the live WebSocket
// stream: blocking the one thread to pace a REST call would stall the read
// loop that the startup buffer depends on.
//
// A live run does NOT exercise the throttling branch. At 16 concurrent
// requests and ~364 ms per request the measured rate is ~44 req/s, under the
// 60 req/s ceiling the default 300-per-5s config allows, so Pump() always
// finds a free slot and the timer never arms. These tests drive it directly
// with a tiny window instead.

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <chrono>
#include <vector>

#include "control_manager/rate_limiter.h"

namespace screener {
namespace {

using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

// Elapsed milliseconds from `start` for each handler, in the order they ran.
std::vector<long> RunAndRecord(uint32_t max_requests, Ms window, int requests) {
    boost::asio::io_context ioc;
    RateLimiter limiter(ioc, max_requests, window);

    std::vector<long> at;
    at.reserve(static_cast<std::size_t>(requests));

    const auto start = Clock::now();
    for (int i = 0; i < requests; ++i) {
        limiter.AsyncAcquire(
            [&at, start]() { at.push_back(std::chrono::duration_cast<Ms>(Clock::now() - start).count()); });
    }

    ioc.run();
    return at;
}

TEST(RateLimiterTest, RunsEveryQueuedHandlerExactlyOnce) {
    const auto at = RunAndRecord(3, Ms(100), 7);
    EXPECT_EQ(at.size(), 7u);
}

TEST(RateLimiterTest, FirstWindowGoesStraightThrough) {
    // Nothing should be delayed while the window has room: the limiter paces,
    // it does not add latency to a request that is already under the limit.
    const auto at = RunAndRecord(3, Ms(200), 3);
    ASSERT_EQ(at.size(), 3u);
    for (const long t : at) {
        EXPECT_LT(t, 50) << "an unthrottled request waited " << t << "ms";
    }
}

TEST(RateLimiterTest, ThrottlesTheOverflowIntoLaterWindows) {
    // max 3 per 100ms, 7 requests -> 3 now, 3 at ~100ms, 1 at ~200ms.
    // This is the branch a live run never reaches.
    const auto at = RunAndRecord(3, Ms(100), 7);
    ASSERT_EQ(at.size(), 7u);

    EXPECT_LT(at[0], 50);
    EXPECT_LT(at[1], 50);
    EXPECT_LT(at[2], 50);

    // Lower bound only on the upper side of the boundary: a loaded CI box can
    // be late, but it must never be EARLY - early means the IP gets banned.
    EXPECT_GE(at[3], 95);
    EXPECT_GE(at[4], 95);
    EXPECT_GE(at[5], 95);
    EXPECT_GE(at[6], 195);
}

TEST(RateLimiterTest, NeverExceedsTheBudgetInAnyWindow) {
    // The property that actually matters. Bybit's unban delay is 10 minutes,
    // so overshooting the window even once is far worse than pacing slowly.
    const uint32_t kMax = 4;
    const auto window = Ms(100);
    const auto at = RunAndRecord(kMax, window, 20);
    ASSERT_EQ(at.size(), 20u);

    for (std::size_t i = 0; i + kMax < at.size(); ++i) {
        // Request i and request i+kMax are kMax+1 requests apart, so they
        // cannot both fall inside one window.
        const long span = at[i + kMax] - at[i];
        EXPECT_GE(span, window.count() - 5) << kMax + 1 << " requests inside one window, starting at index " << i;
    }
}

TEST(RateLimiterTest, ServesHandlersInQueueOrder) {
    // The warm-up logs "N/791" progress and issues requests in symbol order;
    // reordering here would make that log mean nothing.
    boost::asio::io_context ioc;
    RateLimiter limiter(ioc, 2, Ms(50));

    std::vector<int> order;
    for (int i = 0; i < 10; ++i) {
        limiter.AsyncAcquire([&order, i]() { order.push_back(i); });
    }
    ioc.run();

    ASSERT_EQ(order.size(), 10u);
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(order[static_cast<std::size_t>(i)], i);
    }
}

TEST(RateLimiterTest, AHandlerMayQueueTheNextRequest) {
    // How the warm-up actually drives it: each completion tops the pipeline
    // back up. Pump() posts handlers rather than calling them inline, so this
    // re-entry cannot run inside the loop that is still draining waiters_.
    boost::asio::io_context ioc;
    RateLimiter limiter(ioc, 2, Ms(20));

    int done = 0;
    std::function<void()> issue = [&]() {
        if (++done < 12) {
            limiter.AsyncAcquire(issue);
        }
    };
    limiter.AsyncAcquire(issue);

    ioc.run();
    EXPECT_EQ(done, 12);
}

TEST(RateLimiterTest, BlockingAndAsyncShareOneWindow) {
    // FetchUniverse() uses Acquire() before ioc.run(); everything after uses
    // AsyncAcquire(). Two independent windows would let the two paths
    // together exceed the IP limit.
    boost::asio::io_context ioc;
    RateLimiter limiter(ioc, 2, Ms(150));

    const auto start = Clock::now();
    limiter.Acquire();  // fills 1 of 2
    limiter.Acquire();  // fills 2 of 2 - the window is now full

    long first_async = -1;
    limiter.AsyncAcquire([&]() { first_async = std::chrono::duration_cast<Ms>(Clock::now() - start).count(); });
    ioc.run();

    EXPECT_GE(first_async, 145) << "the async path ignored the stamps the blocking path left";
}

}  // namespace
}  // namespace screener
