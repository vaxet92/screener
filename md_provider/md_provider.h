#pragma once

// BybitProvider: the WebSocket side of the screener.
//
// Owns the connection to wss://stream.bybit.com/v5/public/linear, the batched
// {"op":"subscribe"} frames, the 20s client ping timer, the missed-pong
// liveness check, reconnect with exponential backoff and jitter, and the
// parse from Bybit's kline payload into Candle.
//
// THREADING: it owns NO THREAD and NO io_context. Both are handed in by
// ControlManager, and everything here runs as completion handlers on that one
// io_context, on the main thread. That is the whole reason the candle
// callback below can be a direct call into CoreManager: producer and consumer
// are the same thread, so there is nothing to synchronise.

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.h"
#include "md_provider/bybit_parser.h"
#include "md_provider/ws/ws.h"
#include "types/candle.h"

namespace screener {

class BybitProvider {
   public:
    // Called once per CLOSED candle, synchronously, from inside the
    // WebSocket read handler.
    //
    // A direct callback and not an SPSC push. At ~1000 symbols the confirmed
    // bars arrive at ~1000 PER HOUR, with a burst at each hour boundary, and
    // the unconfirmed frames we discard at under ~1000/second - which at 5us
    // of parse per message is about 0.5% of one core. A lock-free ring
    // between two points on the same thread would buy nothing and cost the
    // ability to reason about the code.
    //
    // This is safe ONLY while there is one thread. Moving the parse to its
    // own thread makes this a data race on every indicator, and the callback
    // must become a queue push at the same time (DESIGN.md §13, Phase 1
    // item 10).
    using CandleCallback = std::function<void(const Candle&)>;

    BybitProvider(boost::asio::io_context& ioc, boost::asio::ssl::context& ssl_ctx, const ScreenerConfig& config,
                  CandleCallback on_candle);

    // `symbols` must be index-aligned with CoreManager's state vector: the id
    // this provider stamps into every Candle is the position in this vector.
    void Start(const std::vector<Symbol>& symbols);

    void Stop();

    // Diagnostics, for the console line and for tests.
    uint64_t FramesReceived() const noexcept { return frames_received_; }
    uint64_t CandlesEmitted() const noexcept { return candles_emitted_; }
    uint32_t MissedPongs() const noexcept { return missed_pongs_; }
    uint32_t Reconnects() const noexcept { return reconnects_; }

   private:
    void Connect();
    void OnMessage(std::string_view raw);
    void OnSessionClosed();

    void ScheduleReconnect();

    // Sends {"op":"ping"} and checks whether the PREVIOUS one was answered.
    //
    // The ping is mandatory anyway - Bybit closes a socket that has been
    // quiet for 20s - so using its answer as the liveness signal is free. It
    // also covers the one failure Beast cannot report: a half-open socket
    // where no bytes arrive and no error is ever delivered, so the read
    // handler simply never fires again.
    void SchedulePing();
    void SendPing();

    // One {"op":"subscribe"} per batch of topics_per_subscribe symbols.
    // Bybit caps the args array by CHARACTER count (~21,000), so a 1000-topic
    // frame would sit close enough to the limit to risk silent rejection.
    std::vector<std::string> BuildSubscribeFrames() const;

    boost::asio::io_context& ioc_;
    boost::asio::ssl::context& ssl_ctx_;
    ScreenerConfig config_;
    CandleCallback on_candle_;

    BybitParser parser_;

    // Symbol -> id, built once in Start(). Transparent hash, so the
    // per-message lookup takes a string_view and allocates nothing.
    SymbolIdMap ids_;
    std::vector<std::string> topics_;

    WebSocketSessionSSLPtr session_;
    boost::asio::steady_timer ping_timer_;
    boost::asio::steady_timer reconnect_timer_;

    // Scratch for the parse. A member rather than a local so a steady stream
    // reuses its capacity instead of allocating per frame.
    std::vector<Candle> scratch_;

    bool stopped_ = false;
    bool awaiting_pong_ = false;
    uint32_t missed_pongs_ = 0;
    uint32_t reconnect_attempt_ = 0;
    uint32_t reconnects_ = 0;
    uint64_t frames_received_ = 0;
    uint64_t candles_emitted_ = 0;
};

}  // namespace screener
