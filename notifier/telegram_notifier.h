#pragma once

// TelegramNotifier: fire-and-forget chat notifications for the three events
// an operator wants without tailing the log - startup, a symbol becoming
// ACTIVE, and a symbol leaving ACTIVE.
//
// NOT A DECISION PATH. Every send is best-effort: a failure is logged and
// dropped, never retried, and never reported back to the caller. A Telegram
// outage, a wrong chat_id or a revoked token must not change what the
// screener does with market data.
//
// Runs on the ONE io_context, like everything else. Sends go out through
// AsyncHttpsGet - Telegram's sendMessage takes its parameters in the query
// string, so there is no POST transport to write - which means nothing here
// ever blocks the WebSocket read loop.
//
// PACING, and why there is a queue at all: transitions arrive in BURSTS. The
// filter runs on a closed 1h bar, so at the top of the hour dozens of symbols
// can flip within milliseconds of each other, while Telegram's per-chat limit
// is around 20 messages per minute. Messages therefore go into a FIFO drained
// by a timer, one every telegram_min_send_interval_ms. The queue is capped;
// past the cap a message is dropped and counted, and the count is reported
// into the chat once the queue drains, because a silently missing ACTIVE
// notification is worse than a late one.
//
// This has its OWN pacing timer and deliberately does not share the Bybit
// RateLimiter: different host, different limit, and one shared window would
// let chat traffic eat the kline request budget.

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <cstddef>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "config/config.h"
#include "types/candle.h"

namespace screener {

class TelegramNotifier {
   public:
    // The transport seam. Production hands this an AsyncHttpsGet call; tests
    // hand it a lambda that records the text, so the formatting, the cap, the
    // pacing order and the drop policy are all testable with no network and
    // no bot token.
    using Sender = std::function<void(const std::string& text)>;

    // Disabled unless config.telegram_enabled is set AND both secrets are
    // present. Disabled means every Notify* call is a no-op, not an error:
    // the screener runs identically with and without a bot configured.
    TelegramNotifier(boost::asio::io_context& ioc, boost::asio::ssl::context& ssl_ctx, const ScreenerConfig& config);

    // Test constructor. Same queue, same pacing, injected transport.
    TelegramNotifier(boost::asio::io_context& ioc, const ScreenerConfig& config, Sender sender);

    TelegramNotifier(const TelegramNotifier&) = delete;
    TelegramNotifier& operator=(const TelegramNotifier&) = delete;

    // What warm-up produced. `not_ready` holds one preformatted line per
    // symbol that cannot go ACTIVE, with the reason - "791 tracked, 781
    // ready" otherwise leaves the operator with ten symbols that vanished
    // from the product and no way to find out which.
    struct StartupReport {
        std::size_t tracked = 0;
        std::size_t ready = 0;
        std::vector<std::string> not_ready;
    };

    void NotifyStarted(const StartupReport& report, const std::unordered_set<Symbol>& active);
    void NotifyActivated(const Symbol& symbol, const std::unordered_set<Symbol>& active);
    void NotifyDeactivated(const Symbol& symbol, const std::unordered_set<Symbol>& active);

    // Sends arbitrary text through the same queue and pacing. The manual
    // smoke test uses it; nothing on the candle path does.
    void SendText(std::string text);

    bool Enabled() const noexcept { return enabled_; }

    // Cancels the pacing timer. Whatever is still queued is abandoned - on
    // shutdown the operator is watching the process exit, not the chat.
    void Stop();

    // For tests and the status line.
    std::size_t QueueDepth() const noexcept { return queue_.size(); }
    std::size_t Dropped() const noexcept { return dropped_total_; }

   private:
    // Splits `text` on line boundaries into messages Telegram will accept,
    // then queues each. Line boundaries, not characters, because the text
    // carries HTML tags and a cut inside <b>...</b> is a 400.
    void Enqueue(std::string text);

    // One already-sized message onto the queue.
    void Push(std::string text);

    // Sends the head of the queue and arms the timer for the next one.
    void PumpQueue();

    // "Active (12):" followed by one bold symbol per line.
    //
    // NOT capped by default (telegram_active_list_max = 0). Telegram still
    // rejects a message over 4096 characters, so the length is handled where
    // it belongs - Enqueue splits an oversized message on line boundaries
    // into several, instead of hiding symbols behind "+19 more".
    std::string FormatActive(const std::unordered_set<Symbol>& active) const;

    boost::asio::steady_timer timer_;
    Sender sender_;

    std::string host_;
    std::string port_;
    std::string target_prefix_;  // "/bot<token>/sendMessage?chat_id=<id>&text="

    std::chrono::milliseconds interval_;
    std::size_t max_queue_;
    std::size_t active_list_max_;

    std::deque<std::string> queue_;

    // Dropped since the last report into the chat, and over the whole run.
    std::size_t dropped_pending_ = 0;
    std::size_t dropped_total_ = 0;

    bool enabled_ = false;
    bool timer_armed_ = false;
};

}  // namespace screener
