#pragma once

// ScreenerConfig: every tunable in one place, with defaults that work.
//
// An optional config FILE (config.json) now exists for the filter
// thresholds: ema_period, the surge ratio and the NATR/turnover floors are
// no longer compile-time constants, so there is something an operator can
// legitimately choose before a run. Everything else stays a flag - see
// DESIGN.md for why only the threshold group moved.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "types/candle.h"

namespace screener {

struct ScreenerConfig {
    // ---- Bybit v5 public, USDT perpetuals ---------------------------------
    //
    // `linear` is the USDT/USDC-margined futures category. NEVER
    // /v5/public/spot: spot and futures are different instruments with
    // different turnover, and mixing them makes the surge condition
    // meaningless.
    std::string ws_host = "stream.bybit.com";
    std::string ws_port = "443";
    std::string ws_target = "/v5/public/linear";

    std::string rest_host = "api.bybit.com";
    std::string rest_port = "443";

    // ---- Universe ---------------------------------------------------------

    // Cap the universe. 0 means "every LinearPerpetual that is Trading".
    // Mostly a development aid: a 20-symbol run warms up in seconds rather
    // than minutes.
    uint32_t max_symbols = 0;

    // Topics per {"op":"subscribe"} frame.
    //
    // Bybit documents a 21,000-CHARACTER cap on the args array, not a topic
    // count. At ~20 characters per topic ("kline.60.1000PEPEUSDT") 1000
    // symbols is ~20,000 characters - close enough to the cap that one
    // oversized frame could be silently rejected, so the subscription is
    // batched.
    uint32_t topics_per_subscribe = 100;

    // ---- Warm-up ----------------------------------------------------------

    // LTF bars to fetch per symbol at startup.
    //
    // Sized by the deepest indicator: EMA50 runs on the 4h timeframe, so it
    // needs 50 HTF bars = 200 LTF bars to produce a first value, and several
    // times that to be worth trusting. 600 gives 150 HTF bars; the extra 3
    // cover the leading partial 4h group the aggregator discards.
    //
    // Under Bybit's limit=1000, so this is ONE request per symbol. We never
    // fetch interval=240: the HTF history is built from these same bars by
    // the same aggregator that builds it live, so warm-up and live HTF bars
    // cannot disagree.
    uint32_t warmup_ltf_bars = 603;

    // ---- Liveness ---------------------------------------------------------

    // Client ping interval. Bybit expects a {"op":"ping"} within 20s or it
    // closes the connection, so this is the venue's number, not ours.
    uint32_t ping_interval_s = 20;

    // Consecutive unanswered pings before we reconnect.
    //
    // This is the ONLY liveness check. There is deliberately no data-silence
    // watchdog: one connection carries every symbol, so there is nothing
    // per-symbol to time out, and any "no data for N seconds" threshold would
    // be a guess that flaps a healthy quiet feed. A missed pong is a protocol
    // fact - we sent a request the venue always answers.
    //
    // 3 pings at 20s is ~60s to detect a half-open socket, which is the one
    // failure Beast's error callback cannot see: no bytes arrive and no error
    // is ever reported.
    uint32_t max_missed_pongs = 3;

    uint32_t reconnect_initial_ms = 1000;
    uint32_t reconnect_max_ms = 60000;

    // ---- REST pacing ------------------------------------------------------

    // Bybit's public IP limit is 600 requests / 5s. We self-pace under it and
    // do NOT trust the X-Bapi-Limit-* response headers: those report the
    // per-UID endpoint quota, which is not the limit our unauthenticated
    // market calls are bound by.
    uint32_t rest_max_requests_per_window = 300;
    uint32_t rest_window_ms = 5000;

    // On 403 / retCode 10006 ("access too frequent") the documented unban
    // delay is 10 minutes. Retrying sooner extends the ban, so this is a
    // floor, not a backoff seed.
    uint32_t rest_ban_backoff_ms = 600'000;

    // ---- Telegram notifications ------------------------------------------

    // Off unless --telegram is passed. The screener runs identically with and
    // without a bot: the notifier is a reporting sink, never a decision path.
    bool telegram_enabled = false;

    // Read from `.env` and from the ENVIRONMENT (TELEGRAM_BOT_TOKEN,
    // TELEGRAM_CHAT_ID), never from a flag.
    //
    // A token on the command line is readable by every user on the box
    // through `ps`, and it lands in shell history. That is the one reason
    // this project reads anything other than argv - secrets are the exception
    // to "every tunable is a flag", not a second configuration mechanism.
    //
    // The SHELL WINS over the file: `.env` supplies defaults so a normal run
    // needs no exports, and an explicit export still redirects one run to a
    // test chat without editing the file.
    std::string telegram_token;
    std::string telegram_chat_id;

    // Where those two are read from. A missing default file is fine (it means
    // "use the environment"); a missing file named explicitly with
    // --env-file= is an error, because the operator said where to look.
    std::string env_file = ".env";

    std::string telegram_host = "api.telegram.org";
    std::string telegram_port = "443";

    // Minimum gap between two messages.
    //
    // Transitions arrive in bursts: the filter runs on a closed 1h bar, so
    // dozens of symbols can flip within milliseconds at the top of the hour.
    // Telegram's per-chat limit is around 20 messages/minute, so 3s is a
    // deliberate ~20/minute ceiling rather than a guess.
    uint32_t telegram_min_send_interval_ms = 3000;

    // Queue cap. At 3s apart this is ~5 minutes of backlog, which is far
    // longer than one hourly burst can legitimately be - so hitting it means
    // the chat is hopelessly behind and dropping is the right answer. Drops
    // are counted and reported into the chat.
    uint32_t telegram_max_queue = 100;

    // Symbols listed before the active list is truncated. 0 = ALL of them,
    // which is the default: a truncated list hides exactly the symbol the
    // operator went looking for.
    //
    // Telegram still rejects a message over 4096 characters, but length is
    // handled by splitting an oversized message on line boundaries rather
    // than by dropping symbols. This knob exists for the other problem - a
    // few hundred ACTIVE symbols means every transition re-sends a list two
    // messages long, and at that point capping it is a chat-noise decision,
    // not a protocol limit.
    uint32_t telegram_active_list_max = 0;

    bool verbose = false;

    // ---- Filter thresholds (optional config.json) --------------------------
    //
    // Defaults match the values CoreManager/Ema used as compile-time
    // constants before this file existed. Overridable via config.json only -
    // there is no flag for these, because a threshold typed on a command
    // line is exactly the kind of value that gets forgotten between runs.

    // EMA period on the HTF (4h) close. Was a template parameter
    // (Ema<kEmaPeriod>); now a runtime field the indicator is constructed
    // with. warmup_ltf_bars is NOT auto-derived from this in the file reader
    // - see Validate() for why that would be the wrong place to do it.
    uint32_t ema_period = 50;

    // Volume surge: recent_turnover * surge_denominator > prev_turnover *
    // surge_numerator. Same integer cross-multiplication CoreManager always
    // used; these are now the operands instead of file-scope constants.
    int64_t surge_numerator = 130;
    int64_t surge_denominator = 100;

    // NATR floor, in basis points.
    int32_t min_natr_bp = 100;

    // Liquidity floor: the recent-window turnover sum (same window the surge
    // condition reads) must exceed this, scaled by kVolumeScale. Parsed from
    // a decimal USDT string in config.json straight to a scaled integer -
    // never via a double, same rule as every other price/volume value in
    // this system.
    Volume min_turnover = 0;

    // Where the threshold group above is read from. A missing default file
    // means "use the compiled defaults"; a missing file named explicitly
    // with --config= is an error, same policy as --env-file=.
    std::string config_file = "config.json";

    // Reads --max-symbols=N, --warmup-bars=N, --topics-per-sub=N,
    // --telegram, --env-file=PATH, --config=PATH, --verbose, plus
    // TELEGRAM_BOT_TOKEN / TELEGRAM_CHAT_ID from `.env` and the environment.
    // nullopt on an unrecognised or malformed flag: the caller refuses to
    // start rather than run with a value the operator did not mean.
    static std::optional<ScreenerConfig> FromArgs(int argc, char* argv[]);

    // Empty string when the configuration is usable, otherwise the reason.
    std::string Validate() const;
};

// Applies dotenv CONTENT (not a path) to `config`. Empty return on success,
// otherwise the reason, naming the line - a typo in a secrets file must stop
// the run, exactly like an unrecognised flag.
//
// Deliberately NOT a dotenv implementation: no variable interpolation, no
// multi-line values, no `.env.local` layering. It reads `KEY=VALUE`, one per
// line, with `#` comments, blank lines, an optional `export ` prefix and
// optional surrounding quotes - which is every .env file anyone actually
// hand-writes.
//
// Keys other than TELEGRAM_BOT_TOKEN and TELEGRAM_CHAT_ID are IGNORED, not
// rejected: a .env is shared with whatever else runs in this directory, and
// this process has no business failing on another tool's variable.
//
// Separate from the file reading so the parsing is testable without a
// filesystem.
std::string ApplyDotEnv(std::string_view content, ScreenerConfig& config);

// Applies config.json CONTENT (not a path) to `config`. Empty return on
// success, otherwise the reason. Only the five threshold fields above are
// recognised; any other key is an error, unlike .env - this file has no
// other tool sharing it, so an unrecognised key is a typo the operator
// should hear about, not a quietly ignored line.
//
// EXCEPT a "//"-prefixed key, which is a COMMENT and is ignored. Plain JSON
// has no comment syntax, so this is the one escape from "unrecognised key is
// fatal" - see example_config.json, which uses it to document every field
// without becoming unusable as a direct copy to config.json.
//
// Values are read straight to the field's integer type via simdjson; a
// turnover floor given as a JSON number with a fractional part (e.g.
// 1000000.50) is rejected - see the .cpp for why a string is required for
// that one field instead.
std::string ApplyConfigJson(std::string_view content, ScreenerConfig& config);

}  // namespace screener
