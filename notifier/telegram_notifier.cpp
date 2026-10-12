#include "notifier/telegram_notifier.h"

#include <fmt/chrono.h>
#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

#include "logger/logger.h"
#include "md_provider/async_rest.h"

namespace screener {
namespace {

// Percent-encode everything outside the unreserved set.
//
// The message text goes into a query parameter, and the symbols and counts we
// send are ASCII - but the emoji and the newlines are not, and an unencoded
// newline or '&' would either truncate the message at the first separator or
// produce a 400 from Telegram. Encoding unconditionally is cheaper than
// reasoning about which characters the next message format introduces.
std::string UrlEncode(std::string_view in) {
    static constexpr char kHex[] = "0123456789ABCDEF";

    std::string out;
    out.reserve(in.size() + in.size() / 4);

    for (const char ch : in) {
        const auto byte = static_cast<unsigned char>(ch);
        const bool unreserved = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                                (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' ||
                                byte == '~';
        if (unreserved) {
            out.push_back(ch);
        } else {
            out.push_back('%');
            out.push_back(kHex[byte >> 4]);
            out.push_back(kHex[byte & 0x0F]);
        }
    }
    return out;
}

// Telegram's HTML parse mode needs exactly three characters escaped.
//
// HTML and not MarkdownV2: MarkdownV2 requires 18 characters escaped
// (`_*[]()~`>#+-=|{}.!`), and a single missed one is a 400 on a notification
// nobody sees fail. A symbol name is alphanumeric today, but the counts,
// parentheses and hyphens around it are not, and the next message format will
// introduce more.
std::string HtmlEscape(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (const char ch : in) {
        switch (ch) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            default:
                out.push_back(ch);
        }
    }
    return out;
}

std::string Bold(std::string_view in) {
    return "<b>" + HtmlEscape(in) + "</b>";
}

// Telegram rejects a message over 4096 characters. The margin covers the
// difference between characters and the UTF-16 code units Telegram actually
// counts: an emoji is one character here and two there.
inline constexpr std::size_t kMaxMessageChars = 3800;

// UTC, minute resolution - every message gets one line of this, so
// microsecond precision (Logger::Timestamp()) would just be noise an
// operator reading chat on a phone has to skip past.
std::string FormatUtcTimestamp() {
    const auto now = std::chrono::system_clock::now();
    return fmt::format("\xF0\x9F\x95\x92 {:%Y-%m-%d %H:%M} UTC",
                       fmt::gmtime(std::chrono::system_clock::to_time_t(now)));
}

// Magnitude only, two decimal places, from a basis-point integer - no
// division that would need a float. Used for NATR (always >= 0) and for the
// EMA distance, where the caller supplies the direction word separately.
std::string FormatPercentBp(int64_t bp) {
    const int64_t abs_bp = bp < 0 ? -bp : bp;
    return fmt::format("{}.{:02}%", abs_bp / 100, abs_bp % 100);
}

// Signed whole percent - the surge condition has no fractional threshold, so
// a whole number reads cleaner than two decimals that are never meaningful.
std::string FormatSignedPercent(int64_t pct) {
    return fmt::format("{}{}%", pct >= 0 ? "+" : "", pct);
}

// Basis points of (close - ema) relative to ema. __int128 for the same
// reason CoreManager's surge comparison uses it: diff * 10000 reaches ~1e19
// at BTC scale, which overflows int64 by a small enough margin that it is
// worth not risking.
int64_t TrendBp(Price close, Price ema) {
    if (ema == 0) {
        return 0;
    }
    const __int128 diff = static_cast<__int128>(close - ema) * 10000;
    return static_cast<int64_t>(diff / ema);
}

// Whole percent change of recent vs prev turnover, same 128-bit rationale.
int64_t SurgePercent(Volume recent, Volume prev) {
    if (prev == 0) {
        return 0;
    }
    const __int128 ratio = static_cast<__int128>(recent) * 100;
    return static_cast<int64_t>(ratio / prev) - 100;
}

// "$412,300" - grouped, no locale dependency (fmt's "{:L}" pulls in the
// global locale, which is one more thing to get wrong on a box that never
// sets LC_NUMERIC).
std::string FormatUsd(int64_t amount) {
    const std::string digits = fmt::format("{}", amount < 0 ? -amount : amount);
    std::string grouped;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i != 0 && (digits.size() - i) % 3 == 0) {
            grouped.push_back(',');
        }
        grouped.push_back(digits[i]);
    }
    return fmt::format("${}{}", amount < 0 ? "-" : "", grouped);
}

// The four condition lines, with current values - shown on BOTH ACTIVE and
// INACTIVE messages, so the chat always carries "why", not just "what".
std::string FormatConditionBlock(const TelegramNotifier::TransitionDetail& d) {
    if (!d.gap && !d.ready) {
        return "\xE2\x8F\xB3 Not enough history yet (warming up)\n";
    }

    const int64_t trend_bp = TrendBp(d.last_close, d.ema50);
    const int64_t surge_pct = SurgePercent(d.turnover_recent, d.turnover_prev);

    return fmt::format(
        "\xF0\x9F\x93\x8A Trend: {} {} EMA50\n"
        "\xF0\x9F\x93\x88 Volume surge: {}\n"
        "\xF0\x9F\x8C\x8A Volatility: NATR {}\n"
        "\xF0\x9F\x92\xA7 Liquidity: {} (median {})\n"
        "\xF0\x9F\x94\x80 EMA cross: fast {} slow\n",
        FormatPercentBp(trend_bp), trend_bp >= 0 ? "above" : "below", FormatSignedPercent(surge_pct),
        FormatPercentBp(d.natr_bp), FormatUsd(d.turnover_recent / kVolumeScale),
        FormatUsd(d.turnover_median / kVolumeScale), d.ema_cross ? "above" : "below");
}

// Deactivate-only: WHICH condition(s) broke, against which threshold. A gap
// is not a filter failure, and "not ready" has no threshold to report - both
// get their own one-line reason instead of a condition list.
std::string FormatReason(const TelegramNotifier::TransitionDetail& d) {
    if (d.gap) {
        return "\xE2\x9A\xA0 REASON: data gap detected - rebuilding from REST\n";
    }
    if (!d.ready) {
        return "\xE2\x9A\xA0 REASON: not enough history yet\n";
    }

    std::vector<std::string> failed;
    if (!d.surge) {
        const int64_t surge_pct = SurgePercent(d.turnover_recent, d.turnover_prev);
        const int64_t needed_pct = (d.surge_numerator * 100) / d.surge_denominator - 100;
        failed.push_back(fmt::format("Volume surge {} (needs >{}%)", FormatSignedPercent(surge_pct), needed_pct));
    }
    if (!d.trend) {
        const int64_t trend_bp = TrendBp(d.last_close, d.ema50);
        failed.push_back(fmt::format("Trend: price {} below EMA50 (needs above)", FormatPercentBp(trend_bp)));
    }
    if (!d.volatility) {
        failed.push_back(
            fmt::format("Volatility NATR {} (needs >{})", FormatPercentBp(d.natr_bp), FormatPercentBp(d.min_natr_bp)));
    }
    if (!d.liquidity) {
        failed.push_back(fmt::format("Liquidity median {} (needs \xE2\x89\xA5{})",
                                     FormatUsd(d.turnover_median / kVolumeScale),
                                     FormatUsd(d.min_median_turnover / kVolumeScale)));
    }
    if (!d.ema_cross) {
        failed.push_back("EMA cross: fast EMA below slow EMA (needs above)");
    }
    if (failed.empty()) {
        return "";  // All() true would mean TryDeactivate was never called
    }

    std::string out = "\xE2\x9A\xA0 REASON: ";
    for (std::size_t i = 0; i < failed.size(); ++i) {
        if (i != 0) {
            out += "; ";
        }
        out += failed[i];
    }
    out.push_back('\n');
    return out;
}

}  // namespace

TelegramNotifier::TelegramNotifier(boost::asio::io_context& ioc, boost::asio::ssl::context& ssl_ctx,
                                   const ScreenerConfig& config)
    : TelegramNotifier(ioc, config, Sender{}) {
    if (!enabled_) {
        return;
    }

    // Captures &ioc and &ssl_ctx by reference: both are ControlManager
    // members that outlive this object, and there is one thread.
    //
    // No parse_mode. The text is sent as plain UTF-8, so there is no Markdown
    // or HTML escaping to get wrong - and a symbol name is exactly the kind of
    // string that would eventually contain an underscore and break a
    // Markdown-parsed message with a 400 nobody reads.
    sender_ = [this, &ioc, &ssl_ctx](const std::string& text) {
        AsyncHttpsGet(
            ioc, ssl_ctx, host_, port_, target_prefix_ + UrlEncode(text),
            [](std::optional<std::string> body) {
                // Best-effort: the body is read only to say WHY a send failed.
                // AsyncHttpsGet already logged the transport error or the
                // non-200 status, so there is nothing to do on failure but
                // leave that line in the log.
                if (!body) {
                    Logger::Log(LogLevel::kWarning, "[telegram] send failed - notification dropped");
                }
            },
            // Redacted. The bot token is in the URL PATH, which AsyncHttpsGet
            // prints on every failure - so without this, one network blip
            // writes a live credential into the log.
            "/bot<redacted>/sendMessage");
    };
}

TelegramNotifier::TelegramNotifier(boost::asio::io_context& ioc, const ScreenerConfig& config, Sender sender)
    : timer_(ioc),
      sender_(std::move(sender)),
      host_(config.telegram_host),
      port_(config.telegram_port),
      interval_(config.telegram_min_send_interval_ms),
      max_queue_(config.telegram_max_queue),
      active_list_max_(config.telegram_active_list_max),
      enabled_(config.telegram_enabled && !config.telegram_token.empty() && !config.telegram_chat_id.empty()) {
    if (enabled_) {
        // parse_mode=HTML, for the bold symbol names. Every dynamic value in
        // a message goes through HtmlEscape before it gets here.
        target_prefix_ = fmt::format("/bot{}/sendMessage?chat_id={}&parse_mode=HTML&text=", config.telegram_token,
                                     UrlEncode(config.telegram_chat_id));
        Logger::Log(LogLevel::kInfo, "[telegram] enabled: chat_id={}, one message per {}ms, queue cap {}",
                    config.telegram_chat_id, interval_.count(), max_queue_);
    }
}

void TelegramNotifier::NotifyStarted(const StartupReport& report, const std::unordered_set<Symbol>& active) {
    if (!enabled_) {
        return;
    }

    // `ready` and `not_ready` both, and then WHICH symbols are not ready.
    // A not-ready symbol can never go ACTIVE, so "791 tracked, 781 ready" is
    // ten instruments that silently dropped out of the product - and the
    // difference between "two new listings with no history" and "ten warm-up
    // requests failed" is the difference between fine and broken.
    std::string text = fmt::format("\xF0\x9F\x9A\x80 {}\nTracking {} instruments: {} ready, {} not ready\n",
                                   Bold("Screener started"), report.tracked, report.ready, report.not_ready.size());

    if (!report.not_ready.empty()) {
        text += fmt::format("\n{}\n",
                            Bold(fmt::format("━━━━━━━━━━━━━━━━━━━━\n~~~NOT READY~~~ ({}):", report.not_ready.size())));
        for (const std::string& line : report.not_ready) {
            text += HtmlEscape(line);
            text.push_back('\n');
        }
    }

    text += '\n' + FormatActive(active);
    Enqueue(std::move(text));
}

void TelegramNotifier::NotifyActivated(const Symbol& symbol, const TransitionDetail& detail,
                                       const std::unordered_set<Symbol>& active) {
    if (!enabled_) {
        return;
    }
    const std::string link =
        HtmlEscape(fmt::format("https://www.tradingview.com/chart/?symbol=BYBIT:{}.P&interval=60", symbol));
    Enqueue(fmt::format("\xF0\x9F\x9F\xA2 {}: {}\n\n\xF0\x9F\x93\x88 {}\n\n{}\n{}", Bold("ACTIVE"), Bold(symbol), link,
                        FormatConditionBlock(detail), FormatActive(active)));
}

void TelegramNotifier::NotifyDeactivated(const Symbol& symbol, const TransitionDetail& detail,
                                         const std::unordered_set<Symbol>& active) {
    if (!enabled_) {
        return;
    }
    Enqueue(fmt::format("\xF0\x9F\x94\xB4 {}: {}\n\n{}{}\n{}", Bold("INACTIVE"), Bold(symbol),
                        FormatConditionBlock(detail), FormatReason(detail), FormatActive(active)));
}

void TelegramNotifier::SendText(std::string text) {
    if (!enabled_) {
        return;
    }
    Enqueue(std::move(text));
}

std::string TelegramNotifier::FormatActive(const std::unordered_set<Symbol>& active) const {
    if (active.empty()) {
        return Bold("━━━━━━━━━━━━━━━━━━━━\n~~~ACTIVE~~~ (0):") + " none";
    }

    // Sorted, so the same active set always reads the same way - an
    // unordered_set's iteration order would reshuffle the list on every
    // message and make two consecutive notifications impossible to compare.
    //
    // This allocates and sorts up to ~1000 strings, which is why it is on the
    // TRANSITION path and not the per-bar path: it runs when a symbol flips,
    // at most a few dozen times an hour, never per candle.
    std::vector<std::string_view> names;
    names.reserve(active.size());
    for (const Symbol& s : active) {
        names.emplace_back(s);
    }

    // active_list_max_ == 0 means "all of them", which is the default: a
    // truncated list hides exactly the symbol the operator is looking for,
    // and Enqueue already splits a message that outgrows Telegram's limit.
    const std::size_t shown = active_list_max_ == 0 ? names.size() : std::min(active_list_max_, names.size());
    std::partial_sort(names.begin(), names.begin() + static_cast<std::ptrdiff_t>(shown), names.end());

    std::string out = Bold(fmt::format("━━━━━━━━━━━━━━━━━━━━\n~~~ACTIVE~~~ ({}):", active.size()));
    out.push_back('\n');

    // One per LINE. A comma-separated run of 39 tickers is unreadable on a
    // phone, which is where these are read.
    for (std::size_t i = 0; i < shown; ++i) {
        out += Bold(names[i]);
        out.push_back('\n');
    }
    if (shown < names.size()) {
        out += fmt::format("(+{} more)\n", names.size() - shown);
    }
    return out;
}

void TelegramNotifier::Enqueue(std::string text) {
    // Prepended ONCE, here, so every message - NotifyStarted, a transition,
    // the dropped-notification warning - carries the same UTC header without
    // each call site formatting its own.
    text = FormatUtcTimestamp() + "\n" + text;

    std::string_view rest(text);

    while (!rest.empty()) {
        if (rest.size() <= kMaxMessageChars) {
            Push(std::string(rest));
            return;
        }

        // Cut at the last line break that fits, so no HTML tag is ever split
        // in half - <b>BTC + USDT</b> across two messages is a 400 on both.
        std::size_t cut = rest.rfind('\n', kMaxMessageChars);

        // One line longer than a whole message cannot happen with our formats
        // (the longest is a ticker in <b> tags), but a hard cut is the only
        // safe fallback: looping forever on an unsplittable line would hang
        // the notifier, and dropping it silently is worse.
        if (cut == std::string_view::npos || cut == 0) {
            cut = kMaxMessageChars;
        }

        Push(std::string(rest.substr(0, cut)));
        rest.remove_prefix(cut);

        // Skip the newline the cut landed on, so the next part does not start
        // with a blank line.
        if (!rest.empty() && rest.front() == '\n') {
            rest.remove_prefix(1);
        }
    }
}

void TelegramNotifier::Push(std::string text) {
    if (queue_.size() >= max_queue_) {
        // Drop the NEW message, not the oldest. The queue holds transitions in
        // the order they happened; discarding the head would report the chat's
        // history out of order, which is harder to read than a gap in it.
        ++dropped_pending_;
        ++dropped_total_;
        Logger::Log(LogLevel::kWarning, "[telegram] queue full ({}) - notification dropped ({} total)", max_queue_,
                    dropped_total_);
        return;
    }

    queue_.push_back(std::move(text));

    // The first message goes out immediately; the timer only paces the ones
    // behind it. Waiting a full interval for a single notification would add
    // seconds of latency to the common case, which is one symbol flipping on
    // its own.
    if (!timer_armed_) {
        PumpQueue();
    }
}

void TelegramNotifier::PumpQueue() {
    if (queue_.empty()) {
        timer_armed_ = false;

        // Report the loss once the burst is over, into the chat and not just
        // the log: the whole point of this class is an operator who is not
        // reading the log.
        if (dropped_pending_ != 0) {
            const std::size_t dropped = dropped_pending_;
            dropped_pending_ = 0;
            Enqueue(fmt::format("\xE2\x9A\xA0 {} notification(s) dropped (rate limit)", dropped));
        }
        return;
    }

    const std::string text = std::move(queue_.front());
    queue_.pop_front();
    if (sender_) {
        sender_(text);
    }

    timer_armed_ = true;
    timer_.expires_after(interval_);
    timer_.async_wait([this](const boost::system::error_code& ec) {
        if (ec) {
            timer_armed_ = false;  // cancelled by Stop()
            return;
        }
        PumpQueue();
    });
}

void TelegramNotifier::Stop() {
    timer_.cancel();
    queue_.clear();
}

}  // namespace screener
