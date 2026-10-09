#include "md_provider/md_provider.h"

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <chrono>
#include <random>

#include "logger/logger.h"

namespace screener {
namespace {

// Jitter the reconnect delay by +/-20%.
//
// Without it, every client that lost the same venue-side socket reconnects at
// the same instant, and the venue sees a synchronised stampede - which is how
// a brief outage becomes a rate-limit ban. With one connection this matters
// less than it would with many, but the cost is one random number per
// reconnect.
uint32_t Jitter(uint32_t delay_ms) {
    static std::mt19937 rng{std::random_device{}()};
    const auto span = static_cast<int64_t>(delay_ms) / 5;  // 20%
    if (span <= 0) {
        return delay_ms;
    }
    std::uniform_int_distribution<int64_t> dist(-span, span);
    return static_cast<uint32_t>(static_cast<int64_t>(delay_ms) + dist(rng));
}

}  // namespace

BybitProvider::BybitProvider(boost::asio::io_context& ioc, boost::asio::ssl::context& ssl_ctx,
                             const ScreenerConfig& config, CandleCallback on_candle)
    : ioc_(ioc),
      ssl_ctx_(ssl_ctx),
      config_(config),
      on_candle_(std::move(on_candle)),
      ping_timer_(ioc),
      reconnect_timer_(ioc) {}

void BybitProvider::Start(const std::vector<Symbol>& symbols) {
    ids_.clear();
    topics_.clear();
    ids_.reserve(symbols.size());
    topics_.reserve(symbols.size());

    for (uint32_t id = 0; id < symbols.size(); ++id) {
        ids_.emplace(symbols[id], id);
        // Bybit topic naming: kline.{interval}.{SYMBOL}, interval in MINUTES
        // as a bare number ("60", not "1h"), symbol uppercase. We subscribe
        // to the LTF only - the 4h is aggregated locally.
        topics_.push_back(fmt::format("kline.60.{}", symbols[id]));
    }

    stopped_ = false;
    reconnect_attempt_ = 0;
    Connect();
}

void BybitProvider::Stop() {
    stopped_ = true;
    ping_timer_.cancel();
    reconnect_timer_.cancel();
    if (session_) {
        session_->Stop();
        session_.reset();
    }
}

std::vector<std::string> BybitProvider::BuildSubscribeFrames() const {
    std::vector<std::string> frames;
    const std::size_t batch = config_.topics_per_subscribe;

    for (std::size_t i = 0; i < topics_.size(); i += batch) {
        const std::size_t end = std::min(i + batch, topics_.size());

        // Hand-built rather than via a JSON writer: the shape is fixed and
        // this avoids pulling a serialiser in for one message type.
        std::string frame = R"({"op":"subscribe","args":[)";
        for (std::size_t j = i; j < end; ++j) {
            if (j > i) {
                frame += ',';
            }
            frame += '"';
            frame += topics_[j];
            frame += '"';
        }
        frame += "]}";

        // Per-frame, with the topic list and the BYTE SIZE.
        //
        // The size is the number that matters: Bybit's documented cap is on
        // the args array in characters (~21,000), not on a topic count, and an
        // oversized frame is rejected - which looks exactly like a connection
        // that works but delivers nothing for those symbols. This is also the
        // only place that shows which symbols went into which frame, so a
        // partial ack can be matched to the symbols it covered.
        Logger::Log(LogLevel::kDebug, "[Bybit] subscribe frame {}: {} topics, {} bytes: {}", frames.size(), end - i,
                    frame.size(),
                    fmt::join(topics_.begin() + static_cast<std::ptrdiff_t>(i),
                              topics_.begin() + static_cast<std::ptrdiff_t>(end), ", "));

        frames.push_back(std::move(frame));
    }
    return frames;
}

void BybitProvider::Connect() {
    if (stopped_) {
        return;
    }

    session_ = std::make_shared<WebSocketSessionSSL>(ioc_, ssl_ctx_, [this](std::string_view raw) { OnMessage(raw); });

    session_->SetOnOpen([this]() {
        // A successful handshake resets the backoff. Counting attempts across
        // reconnections that actually worked would make a connection that
        // flaps once an hour eventually give up as though it had never
        // connected at all.
        reconnect_attempt_ = 0;
        awaiting_pong_ = false;
        missed_pongs_ = 0;

        // Logged HERE and not on the handshake: the handshake means the
        // socket is a WebSocket, this means it is open AND we are asking for
        // data. The gap between the two is where a subscribe that is silently
        // rejected for being oversized would hide.
        std::vector<std::string> frames = BuildSubscribeFrames();
        Logger::Log(LogLevel::kInfo, "[Bybit] ws stream opened: subscribing {} topics in {} frame(s)", topics_.size(),
                    frames.size());

        for (std::string& frame : frames) {
            session_->Send(std::move(frame));
        }
        SchedulePing();
    });

    session_->SetOnClosed([this]() { OnSessionClosed(); });

    session_->Run(config_.ws_host.c_str(), config_.ws_port.c_str(), config_.ws_target.c_str());
}

void BybitProvider::OnSessionClosed() {
    if (stopped_) {
        return;
    }
    ping_timer_.cancel();
    ++reconnects_;
    ScheduleReconnect();
}

void BybitProvider::ScheduleReconnect() {
    // Exponential, capped, jittered. 1s, 2s, 4s ... 60s.
    uint64_t delay = config_.reconnect_initial_ms;
    delay <<= std::min<uint32_t>(reconnect_attempt_, 6);
    delay = std::min<uint64_t>(delay, config_.reconnect_max_ms);
    ++reconnect_attempt_;

    const uint32_t jittered = Jitter(static_cast<uint32_t>(delay));
    Logger::Log(LogLevel::kWarning, "[Bybit] reconnecting in {}ms (attempt {})", jittered, reconnect_attempt_);

    reconnect_timer_.expires_after(std::chrono::milliseconds(jittered));
    reconnect_timer_.async_wait([this](const boost::system::error_code& ec) {
        if (ec || stopped_) {
            return;  // cancelled by Stop()
        }
        // The dead session is released here rather than inside its own
        // completion handler: destroying it from OnSessionClosed would free
        // the object whose stack frame is still running.
        session_.reset();
        Connect();
    });
}

void BybitProvider::SchedulePing() {
    if (stopped_) {
        return;
    }
    ping_timer_.expires_after(std::chrono::seconds(config_.ping_interval_s));
    ping_timer_.async_wait([this](const boost::system::error_code& ec) {
        if (ec || stopped_) {
            return;
        }
        SendPing();
    });
}

void BybitProvider::SendPing() {
    if (!session_ || !session_->IsOpen()) {
        return;
    }

    // Check the PREVIOUS ping before sending the next one. Bybit answers
    // every ping, so an unanswered one is evidence about the socket rather
    // than a guess about the market - which is exactly why this replaces a
    // data-silence watchdog instead of complementing it.
    if (awaiting_pong_) {
        ++missed_pongs_;
        Logger::Log(LogLevel::kWarning, "[Bybit] pong missed ({}/{})", missed_pongs_, config_.max_missed_pongs);

        if (missed_pongs_ >= config_.max_missed_pongs) {
            // The failure Beast cannot report: a half-open socket delivers no
            // bytes and no error, so the read handler just never fires again
            // and OnClosed is never called. Only our own unanswered request
            // reveals it.
            Logger::Log(LogLevel::kError, "[Bybit] {} pongs missed - forcing reconnect", missed_pongs_);
            if (session_) {
                session_->Stop();
                session_.reset();
            }
            ++reconnects_;
            ScheduleReconnect();
            return;
        }
    }

    awaiting_pong_ = true;
    session_->Send(R"({"op":"ping"})");
    SchedulePing();
}

void BybitProvider::OnMessage(std::string_view raw) {
    ++frames_received_;

    switch (BybitParser::ClassifyFrame(raw)) {
        case BybitParser::FrameKind::kPong:
            awaiting_pong_ = false;
            missed_pongs_ = 0;
            return;

        case BybitParser::FrameKind::kSubscribeAck:
            Logger::Log(LogLevel::kInfo, "[Bybit] subscribe ack: {}", raw);
            return;

        case BybitParser::FrameKind::kKline:
            break;

        case BybitParser::FrameKind::kUnknown:
            Logger::Log(LogLevel::kDebug, "[Bybit] unhandled frame: {}", raw);
            return;
    }

    scratch_.clear();
    if (!parser_.ParseKlineFrame(raw, ids_, scratch_)) {
        Logger::Log(LogLevel::kError, "[Bybit] malformed kline frame");
        return;
    }

    for (const Candle& c : scratch_) {
        ++candles_emitted_;
        // Direct, synchronous call into CoreManager. Safe only because this
        // is the one and only thread - see the header.
        on_candle_(c);
    }
}

}  // namespace screener
