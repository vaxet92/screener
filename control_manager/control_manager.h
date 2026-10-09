#pragma once

// ControlManager: startup, REST, and the one io_context.
//
// Owns the lifecycle; CoreManager owns the state. They share the thread but
// not the job: all exchange I/O is here, so md_core never talks to a venue
// and never blocks.
//
// Startup order, and why it is this order:
//
//   1. REST instruments-info (cursor-paged)  -> the symbol universe
//   2. CoreManager::SetUniverse              -> ids are indices from here on
//   3. WS connect + batched subscribe        -> frames start arriving NOW
//   4. REST kline interval=60 per symbol     -> warm-up, LTF ONLY, ASYNC
//   5. ioc.run() drives 3 and 4 concurrently
//   6. last warm-up response -> replay the buffer -> warmed_up_ = true
//
// The subscribe comes BEFORE the warm-up on purpose. A closed bar is pushed
// exactly once, so a bar closing during warm-up while we are unsubscribed is
// gone for good: the symbol then runs one bar stale until its NEXT bar is
// classified as a gap, which is up to an hour later. Subscribing first turns
// that into a buffered bar and costs nothing, because `open_time` already
// decides what to keep - a buffered bar the REST history also covers is
// kDuplicate/kStale and is dropped, one past it is kNext and is applied.
//
// That only works if the read loop keeps running while history is in flight,
// which is why warm-up is async (md_provider/async_rest.h) and the rate
// limiter grew AsyncAcquire. On one thread a blocking fetch would starve
// exactly the socket it is trying to protect.

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "config/config.h"
#include "control_manager/rate_limiter.h"
#include "md_core/core.h"
#include "md_provider/bybit_parser.h"
#include "md_provider/md_provider.h"
#include "types/candle.h"

namespace screener {

class ControlManager {
   public:
    explicit ControlManager(const ScreenerConfig& config);

    // 0 on a clean shutdown, non-zero if startup failed.
    int Run();

   private:
    // true once a kline fetch has been parsed and applied.
    using KlineHandler = std::function<void(bool)>;

    // Pages through instruments-info. Pagination is MANDATORY: the endpoint
    // defaults to 500 entries and there are more than 500 linear symbols, so
    // a single default request silently truncates the universe - and a
    // truncated universe is not an error anyone would notice.
    //
    // The one blocking REST call in the process. It runs before ioc.run(),
    // so there is no event loop to stall, and nothing can overlap with it
    // anyway: ids come from its result, so neither the subscribe nor the
    // warm-up can start until it returns.
    bool FetchUniverse(std::vector<Symbol>& out);

    // ---- Warm-up ----------------------------------------------------------

    void StartWarmup();

    // Tops the in-flight set back up to kWarmupConcurrency. Called once to
    // prime the pipeline and once per completed response.
    void PumpWarmup();

    void OnWarmupDone(bool ok);

    // Flips warmed_up_, replays the startup buffer, and drains any rebuild
    // the replay queued. Runs from the LAST warm-up completion handler, not
    // from inside ApplyCandle, so there is no re-entrancy.
    void FinishWarmup();

    // ---- Shared REST path -------------------------------------------------

    // One async kline request for one symbol, paced through the limiter.
    // Used by both warm-up and rebuild: same endpoint, same parse, same
    // CoreManager::Warmup replay, so the two can never disagree.
    void FetchKline(uint32_t symbol_id, KlineHandler done);

    bool ApplyKlineBody(uint32_t symbol_id, const std::string& body);

    // Drains CoreManager's pending-rebuild list. Posted to the io_context by
    // the candle callback, never called from inside it - a rebuild replays
    // ~600 bars through the warm-up path, and doing that from inside
    // ApplyCandle would re-enter the state that call is still reading.
    void DrainRebuilds();

    void PostRebuildDrain();

    // ---- Live path --------------------------------------------------------

    // Per-symbol readiness after the replay. Loud only for NOT READY.
    void LogWarmupReadiness() const;

    // The provider's candle callback. Buffers until warmed_up_.
    void OnCandle(const Candle& c);

    void ScheduleStatus();

    void StopWith(int exit_code);

    ScreenerConfig config_;

    boost::asio::io_context ioc_;
    boost::asio::ssl::context ssl_ctx_;
    boost::asio::steady_timer status_timer_;
    boost::asio::signal_set signals_;

    RateLimiter limiter_;
    BybitParser rest_parser_;

    CoreManager core_;
    std::unique_ptr<BybitProvider> provider_;

    // Scratch for a warm-up/rebuild fetch, reused across symbols so ~1000
    // warm-ups do not do ~1000 vector growth cycles.
    //
    // Still safe with requests in flight concurrently: one thread, and a
    // completion handler clears, parses and applies without ever suspending,
    // so two handlers can never be inside it at the same time.
    std::vector<Candle> warmup_scratch_;

    // ---- Startup buffer ---------------------------------------------------

    // Candles that arrived before the warm-up finished, in ARRIVAL order.
    // Replayed through ApplyCandle in FinishWarmup; no dedup logic here,
    // because Classify() already does it by open_time.
    std::vector<Candle> startup_buffer_;
    std::size_t startup_dropped_ = 0;

    bool warmed_up_ = false;

    uint32_t warmup_next_id_ = 0;
    std::size_t warmup_in_flight_ = 0;
    std::size_t warmup_done_ = 0;
    std::size_t warmup_ok_ = 0;
    std::chrono::steady_clock::time_point warmup_started_{};

    bool rebuild_drain_posted_ = false;

    // Set from an async handler, returned by Run(). Startup failures are no
    // longer on Run()'s stack - "no symbol warmed up" is now discovered in a
    // completion handler, which can only stop the loop, not return a code.
    int exit_code_ = 0;
};

}  // namespace screener
