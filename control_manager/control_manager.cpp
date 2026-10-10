#include "control_manager/control_manager.h"

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <boost/asio/post.hpp>
#include <chrono>
#include <utility>

#include "logger/logger.h"
#include "md_provider/async_rest.h"
#include "md_provider/rest.h"
#include "md_provider/ws/root_certificates.hpp"

namespace screener {
namespace {

// How many warm-up kline requests may be in flight at once.
//
// The rate limiter is the binding constraint, not this number. At the default
// 300 requests / 5s the ceiling is 60 req/s; a request takes roughly 150 ms
// end to end (TCP + TLS + a ~603-bar response), so 60 req/s needs about 9
// concurrent. 16 keeps the limiter saturated with headroom for a slow
// response without piling up connections that would just sit in the queue.
//
// Startup cost for ~800 symbols is then limiter-bound at ~13 s, down from
// ~2 minutes sequential - which is the whole point: it shrinks the window in
// which a bar can close mid-warm-up by about 10x, and the startup buffer
// covers what is left.
inline constexpr std::size_t kWarmupConcurrency = 16;

// Hard cap on the startup buffer.
//
// At 1h bars a 13-second warm-up can produce at most one bar per symbol, so
// ~800 is the realistic worst case (~38 KB). This cap is three orders of
// magnitude of slack: hitting it means warm-up has been running for hours,
// and dropping those bars is correct because the gap path will rebuild the
// affected symbols anyway.
inline constexpr std::size_t kMaxStartupBuffer = 65536;

}  // namespace

ControlManager::ControlManager(const ScreenerConfig& config)
    : config_(config),
      ioc_(),
      ssl_ctx_(boost::asio::ssl::context::tlsv12_client),
      status_timer_(ioc_),
      signals_(ioc_, SIGINT, SIGTERM),
      limiter_(ioc_, config.rest_max_requests_per_window, std::chrono::milliseconds(config.rest_window_ms)),
      notifier_(ioc_, ssl_ctx_, config) {
    load_root_certificates(ssl_ctx_);
    ssl_ctx_.set_verify_mode(boost::asio::ssl::verify_peer);
}

bool ControlManager::FetchUniverse(std::vector<Symbol>& out) {
    std::string cursor;

    for (int page = 0;; ++page) {
        std::string target = "/v5/market/instruments-info?category=linear&limit=1000";
        if (!cursor.empty()) {
            target += "&cursor=" + cursor;
        }

        // Blocking, and the only blocking call left. Safe because ioc_.run()
        // has not been entered yet.
        limiter_.Acquire();
        const auto body = HttpsGet(config_.rest_host, config_.rest_port, target);
        if (!body) {
            Logger::Log(LogLevel::kError, "instruments-info page {} failed", page);
            return false;
        }

        std::string next;
        if (!rest_parser_.ParseInstruments(*body, out, next)) {
            Logger::Log(LogLevel::kError, "instruments-info page {} unparseable", page);
            return false;
        }

        Logger::Log(LogLevel::kInfo, "instruments-info page {}: {} symbols so far", page, out.size());

        // An empty nextPageCursor is the documented end of the listing.
        if (next.empty()) {
            break;
        }
        cursor = std::move(next);
    }

    if (config_.max_symbols != 0 && out.size() > config_.max_symbols) {
        out.resize(config_.max_symbols);
    }
    return !out.empty();
}

bool ControlManager::ApplyKlineBody(uint32_t symbol_id, const std::string& body) {
    warmup_scratch_.clear();
    if (!rest_parser_.ParseRestKline(body, symbol_id, warmup_scratch_)) {
        Logger::Log(LogLevel::kError, "kline for {} unparseable", core_.NameOf(symbol_id));
        return false;
    }

    // ParseRestKline already reversed Bybit's newest-first ordering, so these
    // are oldest-first - which is what the recursive indicators require.
    // CoreManager::Warmup resets the symbol, replays them, clears
    // rebuild_pending_ and re-evaluates.
    core_.Warmup(symbol_id, warmup_scratch_);
    return true;
}

void ControlManager::FetchKline(uint32_t symbol_id, KlineHandler done) {
    // interval=60 ONLY. The 4h history is built from these bars by the same
    // aggregator that builds it live, so there is no interval=240 request and
    // no second code path for warm-up and live HTF bars to disagree across.
    std::string target = fmt::format("/v5/market/kline?category=linear&symbol={}&interval=60&limit={}",
                                     core_.NameOf(symbol_id), config_.warmup_ltf_bars);

    limiter_.AsyncAcquire([this, symbol_id, target = std::move(target), done = std::move(done)]() mutable {
        Logger::Log(LogLevel::kDebug, "warm-up request sent: {}", core_.NameOf(symbol_id));

        AsyncHttpsGet(ioc_, ssl_ctx_, config_.rest_host, config_.rest_port, std::move(target),
                      [this, symbol_id, done = std::move(done)](std::optional<std::string> body) {
                          if (body) {
                              Logger::Log(LogLevel::kDebug, "warm-up response received: {} ({} bytes)",
                                          core_.NameOf(symbol_id), body->size());
                          } else {
                              // kWarning, and it names the symbol. AsyncHttpsGet
                              // logs the transport error, but only by URL - and a
                              // symbol that never warmed up can never go ACTIVE,
                              // so it drops out of the product silently.
                              Logger::Log(LogLevel::kWarning, "warm-up response FAILED: {}", core_.NameOf(symbol_id));
                          }

                          const bool ok = body.has_value() && ApplyKlineBody(symbol_id, *body);
                          done(ok);
                      });
    });
}

void ControlManager::StartWarmup() {
    warmup_started_ = std::chrono::steady_clock::now();
    Logger::Log(LogLevel::kInfo, "warm-up starting: {} symbols, {} bars each, {} concurrent", core_.SymbolCount(),
                config_.warmup_ltf_bars, kWarmupConcurrency);
    PumpWarmup();
}

void ControlManager::PumpWarmup() {
    while (warmup_in_flight_ < kWarmupConcurrency && warmup_next_id_ < core_.SymbolCount()) {
        const uint32_t id = warmup_next_id_++;
        ++warmup_in_flight_;
        FetchKline(id, [this](bool ok) { OnWarmupDone(ok); });
    }
}

void ControlManager::OnWarmupDone(bool ok) {
    --warmup_in_flight_;
    ++warmup_done_;
    if (ok) {
        ++warmup_ok_;
    }

    if (warmup_done_ % 100 == 0) {
        Logger::Log(LogLevel::kInfo, "warm-up {}/{} ({} buffered)", warmup_done_, core_.SymbolCount(),
                    startup_buffer_.size());
    }

    if (warmup_done_ == core_.SymbolCount()) {
        FinishWarmup();
        return;
    }
    PumpWarmup();
}

void ControlManager::FinishWarmup() {
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - warmup_started_)
            .count();

    // Wall time and request count are on the measurement list: fetching LTF
    // only is one request per symbol instead of two, but ~603 bars of JSON
    // per symbol is more BYTES than a 48+200 split would have been. That
    // trade is the thing worth measuring rather than asserting.
    Logger::Log(LogLevel::kInfo, "warm-up complete: {}/{} symbols in {}ms ({} requests, {} concurrent)", warmup_ok_,
                core_.SymbolCount(), elapsed_ms, warmup_done_, kWarmupConcurrency);

    if (warmup_ok_ == 0) {
        Logger::Log(LogLevel::kError, "no symbol warmed up - stopping");
        StopWith(1);
        return;
    }

    // Order matters: flip the flag BEFORE the replay. OnCandle consults it,
    // and a frame delivered between the first replayed bar and the last must
    // go straight to ApplyCandle, not back onto a buffer nobody will drain
    // again. (It cannot actually happen on one thread - no read handler runs
    // during this loop - but a flag that is only correct because of the
    // threading model is a trap for whoever moves this off one thread.)
    warmed_up_ = true;

    if (startup_dropped_ != 0) {
        Logger::Log(LogLevel::kWarning, "startup buffer overflowed: {} candle(s) dropped (gap path will recover them)",
                    startup_dropped_);
    }

    // Replay in ARRIVAL order, which is per-symbol open_time order because
    // the stream delivers a symbol's bars in sequence. No skip logic here:
    // ApplyCandle classifies each bar by open_time, so one the REST history
    // already covers is kDuplicate/kStale and is dropped, and one past it is
    // kNext and is applied. That is the whole reconciliation.
    Logger::Log(LogLevel::kInfo, "replaying {} buffered candle(s)", startup_buffer_.size());
    for (const Candle& c : startup_buffer_) {
        core_.ApplyCandle(c);
    }
    startup_buffer_.clear();
    startup_buffer_.shrink_to_fit();

    // After the replay, not before: a buffered bar can be the one that tips a
    // symbol over its readiness threshold.
    const TelegramNotifier::StartupReport report = LogWarmupReadiness();

    // The startup notification goes out HERE, not from Run().
    //
    // "Tracking N instruments" is only meaningful once the universe is warm:
    // before this point every symbol is `not ready` and none of them could
    // have gone ACTIVE, so an earlier message would report a count the
    // screener cannot act on yet. It also carries the symbols that came out
    // of warm-up already ACTIVE, which is the state the chat would otherwise
    // never be told about - those are not transitions, so no ACTIVE message
    // will ever be sent for them.
    notifier_.NotifyStarted(report, core_.ActiveInstruments());

    // A replayed bar can be a genuine gap - a symbol whose warm-up failed, or
    // one that lost more than one bar - so drain whatever the replay queued.
    PostRebuildDrain();
}

TelegramNotifier::StartupReport ControlManager::LogWarmupReadiness() const {
    TelegramNotifier::StartupReport report;
    report.tracked = core_.SymbolCount();

    std::size_t ready = 0;
    std::size_t not_ready = 0;
    std::size_t pass_surge = 0;
    std::size_t pass_trend = 0;
    std::size_t pass_vol = 0;
    std::size_t pass_all = 0;

    for (uint32_t id = 0; id < core_.SymbolCount(); ++id) {
        const SymbolTracker& s = core_.TrackerAt(id);
        const uint32_t ltf_bars = s.Ltf().Bars();
        const uint32_t htf_bars = s.Htf().Bars();
        const CoreManager::Conditions c = core_.EvaluateConditions(id);

        if (c.ready) {
            ++ready;
            pass_surge += c.surge ? 1 : 0;
            pass_trend += c.trend ? 1 : 0;
            pass_vol += c.volatility ? 1 : 0;
            pass_all += c.All() ? 1 : 0;

            // The per-condition breakdown, not just "READY". "0 ACTIVE of 10"
            // is otherwise indistinguishable from a broken filter, and the
            // three conditions are ANDed - so knowing WHICH one rejects a
            // symbol is the difference between "the market is quiet" and "the
            // surge comparison has its operands the wrong way round".
            Logger::Log(LogLevel::kDebug,
                        "{}: warm-up complete, READY (ltf={} bars, htf={} bars) surge={} trend={} vol={} -> {}",
                        core_.NameOf(id), ltf_bars, htf_bars, c.surge, c.trend, c.volatility,
                        c.All() ? "ACTIVE" : "inactive");
            continue;
        }

        ++not_ready;

        // The REASON, per symbol, not just the count. Two causes look
        // identical in a bare number and are not remotely the same problem:
        // a brand-new listing genuinely has no history, while ltf=0 means its
        // warm-up REQUEST failed and the symbol is missing for a reason we
        // could fix.
        report.not_ready.push_back(ltf_bars == 0
                                       ? fmt::format("{} - no history (warm-up request failed)", core_.NameOf(id))
                                       : fmt::format("{} - {} x 1h, {} x 4h (EMA{} needs {} x 4h)", core_.NameOf(id),
                                                     ltf_bars, htf_bars, kEmaPeriod, kEmaPeriod));
        // kWarning rather than kDebug, because this is the quiet failure mode
        // of the whole system: a not-ready symbol can never go ACTIVE, so it
        // vanishes from the output with no error anywhere. At ~800 symbols the
        // READY lines would bury it, which is why only this branch is loud.
        Logger::Log(LogLevel::kWarning, "{}: warm-up complete, NOT READY (ltf={} bars, htf={} bars; EMA{} needs {})",
                    core_.NameOf(id), ltf_bars, htf_bars, kEmaPeriod, kEmaPeriod);
    }

    // One line that answers "why is nothing ACTIVE". The three conditions are
    // ANDed, so if `all` is 0 while each condition passes somewhere, the
    // filter is working and the universe is just quiet. If a condition is 0
    // across every symbol, that condition is the suspect.
    Logger::Log(LogLevel::kInfo,
                "readiness: {} ready, {} not ready, of {} symbols | of the ready: surge={} trend={} vol={} all={}",
                ready, not_ready, core_.SymbolCount(), pass_surge, pass_trend, pass_vol, pass_all);

    report.ready = ready;
    return report;
}

void ControlManager::OnCandle(const Candle& c) {
    if (!warmed_up_) {
        if (startup_buffer_.size() < kMaxStartupBuffer) {
            startup_buffer_.push_back(c);
        } else {
            ++startup_dropped_;
        }
        return;
    }

    core_.ApplyCandle(c);
    PostRebuildDrain();
}

void ControlManager::PostRebuildDrain() {
    // POST, not a direct call. DrainRebuilds issues REST fetches whose
    // handlers replay ~600 bars through CoreManager::Warmup; starting that
    // here would mutate state that the ApplyCandle call still on the stack is
    // in the middle of reading. The post runs after the read handler returns,
    // which is the cheapest possible fix for re-entrancy: one flag and one
    // queued function.
    if (rebuild_drain_posted_) {
        return;
    }
    rebuild_drain_posted_ = true;
    boost::asio::post(ioc_, [this]() { DrainRebuilds(); });
}

void ControlManager::DrainRebuilds() {
    rebuild_drain_posted_ = false;

    // Drained HERE, next to the rebuild list, because this already runs
    // AFTER the ApplyCandle call has returned (it is `post`ed) - sending from
    // inside TryActivate would put chat formatting and a URL-encode on the
    // message path and give md_core a dependency on network I/O.
    for (const auto& t : core_.TakeTransitions()) {
        if (t.active) {
            notifier_.NotifyActivated(core_.NameOf(t.symbol_id), core_.ActiveInstruments());
        } else {
            notifier_.NotifyDeactivated(core_.NameOf(t.symbol_id), core_.ActiveInstruments());
        }
    }

    const std::vector<uint32_t> pending = core_.TakePendingRebuilds();
    for (const uint32_t id : pending) {
        Logger::Log(LogLevel::kWarning, "rebuilding {}", core_.NameOf(id));
        FetchKline(id, [this, id](bool ok) {
            if (!ok) {
                // NOTE: rebuild_pending_ stays set, so this symbol's bars
                // keep being dropped rather than applied to known-wrong
                // indicator state - fail safe. But RequestRebuild() also
                // early-returns while the flag is set, so nothing re-queues
                // it and the symbol never recovers. Pre-existing; needs a
                // CoreManager::ClearRebuildPending to fix properly.
                Logger::Log(LogLevel::kError, "rebuild failed for {} - stays INACTIVE", core_.NameOf(id));
            }
        });
    }
}

void ControlManager::ScheduleStatus() {
    status_timer_.expires_after(std::chrono::seconds(60));
    status_timer_.async_wait([this](const boost::system::error_code& ec) {
        if (ec) {
            return;
        }
        if (!warmed_up_) {
            Logger::Log(LogLevel::kInfo, "--- warming up {}/{} | {} buffered | frames={} ---", warmup_done_,
                        core_.SymbolCount(), startup_buffer_.size(), provider_->FramesReceived());
            ScheduleStatus();
            return;
        }
        const auto& active = core_.ActiveInstruments();
        Logger::Log(LogLevel::kInfo, "--- {} ACTIVE of {} | frames={} candles={} reconnects={} ---", active.size(),
                    core_.SymbolCount(), provider_->FramesReceived(), provider_->CandlesEmitted(),
                    provider_->Reconnects());
        if (!active.empty()) {
            Logger::Log(LogLevel::kInfo, "    {}", fmt::join(active, ", "));
        }
        ScheduleStatus();
    });
}

void ControlManager::StopWith(int exit_code) {
    exit_code_ = exit_code;
    notifier_.Stop();
    provider_->Stop();
    status_timer_.cancel();
    ioc_.stop();
}

int ControlManager::Run() {
    // Local, not a member. CoreManager already stores the universe (ids ARE
    // indices into it, and NameOf reads it back), so a second copy here would
    // be a second source of truth for the id->symbol mapping with nothing
    // keeping the two in step.
    std::vector<Symbol> symbols;

    if (!FetchUniverse(symbols)) {
        Logger::Log(LogLevel::kError, "could not build the symbol universe");
        return 1;
    }
    Logger::Log(LogLevel::kInfo, "universe: {} linear USDT perpetuals", symbols.size());

    // Ids are positions in this vector from here on, in CoreManager and in
    // the provider alike. Both are built from the same vector in the same
    // order, which is what makes a Candle's symbol_id a valid index into
    // CoreManager's state without any lookup.
    core_.SetUniverse(symbols);

    // Constructed BEFORE Start() - provider_ is a unique_ptr and calling
    // through it first would dereference null.
    provider_ = std::make_unique<BybitProvider>(ioc_, ssl_ctx_, config_, [this](const Candle& c) { OnCandle(c); });

    signals_.async_wait([this](const boost::system::error_code&, int sig) {
        Logger::Log(LogLevel::kInfo, "signal {} - shutting down", sig);
        StopWith(0);
    });

    // Both of these only QUEUE work; nothing moves until ioc_.run() below.
    // Start() queues the connect, so the subscribe and the first warm-up
    // responses race each other on the event loop - which is exactly the
    // overlap we want. Everything arriving before FinishWarmup lands in
    // startup_buffer_.
    provider_->Start(symbols);
    StartWarmup();
    ScheduleStatus();

    // One io_context, one thread, and this is it. Every WebSocket read, every
    // REST completion, every timer and every rebuild runs here, in sequence -
    // which is the entire reason the provider can call into CoreManager
    // directly.
    ioc_.run();

    Logger::Log(LogLevel::kInfo, "stopped");
    return exit_code_;
}

}  // namespace screener
