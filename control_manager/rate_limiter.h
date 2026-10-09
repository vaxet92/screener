#pragma once

// RateLimiter: a sliding-window pacer for the REST calls.
//
// Bybit's public limit is 600 requests / 5s PER IP. We self-pace well under
// it and deliberately do NOT read the X-Bapi-Limit-* response headers: those
// report the per-UID endpoint quota, which is not the limit our
// unauthenticated market calls are bound by. Trusting them would mean pacing
// against a number that does not apply.
//
// Two interfaces, because there are two kinds of caller:
//
//   Acquire()      blocks the calling thread. Valid ONLY before ioc.run() -
//                  the universe fetch, which has nothing to overlap with.
//   AsyncAcquire() invokes a handler on the io_context when a slot frees.
//                  Everything after the event loop starts uses this, because
//                  blocking the one thread would stall the WebSocket read
//                  loop that the startup buffer depends on.
//
// Both share one window, so mixing them cannot exceed the budget.

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <thread>
#include <utility>

namespace screener {

class RateLimiter {
   public:
    RateLimiter(boost::asio::io_context& ioc, uint32_t max_requests, std::chrono::milliseconds window)
        : ioc_(ioc), timer_(ioc), max_requests_(max_requests), window_(window) {}

    RateLimiter(const RateLimiter&) = delete;
    RateLimiter& operator=(const RateLimiter&) = delete;

    // Blocks until a request may be sent. Startup only - see the header note.
    void Acquire() {
        Prune(std::chrono::steady_clock::now());

        if (stamps_.size() >= max_requests_) {
            // Sleep exactly until the oldest stamp leaves the window, not a
            // fixed interval: a fixed sleep either paces slower than
            // necessary over ~1000 requests, or undershoots and gets the IP
            // banned for 10 minutes.
            const auto wake = stamps_.front() + window_;
            std::this_thread::sleep_until(wake);
            stamps_.pop_front();
        }

        stamps_.push_back(std::chrono::steady_clock::now());
    }

    // Runs `handler` on the io_context as soon as the window allows it.
    // Handlers are served strictly in the order they were queued, so a
    // warm-up of ~800 symbols proceeds in symbol order and the progress log
    // means what it says.
    void AsyncAcquire(std::function<void()> handler) {
        waiters_.push_back(std::move(handler));

        // If the timer is already armed, the window is full and Pump() will
        // run when it expires. Calling Pump() here as well would re-arm the
        // timer on every queued request - harmless but pointless churn.
        if (!timer_armed_) {
            Pump();
        }
    }

    std::size_t Queued() const noexcept { return waiters_.size(); }

   private:
    void Prune(std::chrono::steady_clock::time_point now) {
        while (!stamps_.empty() && now - stamps_.front() >= window_) {
            stamps_.pop_front();
        }
    }

    void Pump() {
        Prune(std::chrono::steady_clock::now());

        while (!waiters_.empty() && stamps_.size() < max_requests_) {
            std::function<void()> handler = std::move(waiters_.front());
            waiters_.pop_front();
            stamps_.push_back(std::chrono::steady_clock::now());

            // post() rather than calling it here. A handler issues a request
            // whose completion queues the next one, and running it inline
            // would put that whole chain on this stack frame, inside a loop
            // that is still mutating waiters_.
            boost::asio::post(ioc_, std::move(handler));
        }

        if (waiters_.empty()) {
            timer_armed_ = false;
            return;
        }

        // The window is full and someone is waiting. Wake exactly when the
        // oldest stamp expires - that is the first instant a slot exists.
        timer_armed_ = true;
        timer_.expires_at(stamps_.front() + window_);
        timer_.async_wait([this](const boost::system::error_code& ec) {
            timer_armed_ = false;
            if (ec) {  // cancelled on shutdown
                return;
            }
            Pump();
        });
    }

    boost::asio::io_context& ioc_;
    boost::asio::steady_timer timer_;

    std::size_t max_requests_;
    std::chrono::milliseconds window_;

    std::deque<std::chrono::steady_clock::time_point> stamps_;
    std::deque<std::function<void()>> waiters_;
    bool timer_armed_ = false;
};

}  // namespace screener
