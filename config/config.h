#pragma once

// ScreenerConfig: every tunable in one place, with defaults that work.
//
// No config FILE. One venue, one market, one timeframe pair - there is
// nothing an operator must choose before the first run, so a file would only
// be a second place for the truth to live and disagree with this one. The few
// values worth changing are command-line flags.

#include <cstdint>
#include <optional>
#include <string>

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

    bool verbose = false;

    // Reads --max-symbols=N, --warmup-bars=N, --topics-per-sub=N, --verbose.
    // nullopt on an unrecognised or malformed flag: the caller refuses to
    // start rather than run with a value the operator did not mean.
    static std::optional<ScreenerConfig> FromArgs(int argc, char* argv[]);

    // Empty string when the configuration is usable, otherwise the reason.
    std::string Validate() const;
};

}  // namespace screener
