#pragma once

// Candle: the ONE value type that crosses the md_provider -> md_core
// boundary. Everything venue-specific (Bybit's `confirm`, `start`, the
// array-of-arrays REST shape, uppercase symbols) stays behind that boundary;
// md_core only ever sees these.

#include <cstdint>
#include <string>
#include <type_traits>

namespace screener {

// Scaled integers, never floating point - see DESIGN.md §3.
//
// Two scales, each sized for its own job:
//
//   kPriceScale 1e10  the perp universe spans ~10^10 (BTC ~1e5 down to the
//                     1000-multiplier contracts at ~1e-6). 1e8 would leave a
//                     micro-priced perp only 3 significant digits and quantise
//                     NATR to ~17bp against a 100bp threshold.
//
//   kVolumeScale 1e6  turnover is always USDT, so it needs no magnitude
//                     headroom - but it is SUMMED over 24 bars, so it needs
//                     overflow headroom. 24 bars at BTC scale is ~2.4e16 here;
//                     at 1e8 it would be ~2.4e18 and the x130 surge
//                     comparison would overflow int64.
using Price = int64_t;   // real price    x kPriceScale
using Volume = int64_t;  // real turnover x kVolumeScale

inline constexpr int64_t kPriceScale = 10'000'000'000;  // 1e10
inline constexpr int64_t kVolumeScale = 1'000'000;      // 1e6

// Logging and the active set only. NEVER on the message path - a Candle
// carries a uint32_t id and dispatch is an array index.
using Symbol = std::string;

// Only the two timeframes this system has. Deliberately not M1..D1: an
// enumerator nobody handles is a branch someone will assume is supported, and
// it costs a -Wswitch warning the moment it is added.
enum class TimeFrame : uint8_t {
    kH1 = 0,  // LTF - what we subscribe to
    kH4 = 1,  // HTF - aggregated locally, never subscribed
};

inline constexpr int64_t kLtfMs = 3'600'000;   // 1h
inline constexpr int64_t kHtfMs = 4 * kLtfMs;  // 4h
inline constexpr int kLtfBarsPerHtf = 4;

struct Candle {
    int64_t open_time_ms;          //  8  IDENTITY: dedup, gap detection, HTF alignment
    int64_t event_time_ms;         //  8  envelope `ts` of the message that CLOSED this bar
    Price open, high, low, close;  // 32  int64, x kPriceScale
    Volume turnover;               //  8  int64, x kVolumeScale
    uint32_t symbol_id;            //  4  index into CoreManager's state vector
    uint32_t reserved_;            //  4  explicit, so the padding is a decision
};

// Trivially copyable is what lets this be memcpy'd into a ring buffer in
// Phase 1 without a per-element constructor call. The sizeof == 64 assert is
// deliberately NOT here: natural layout is a Phase 0 decision (CLAUDE.md).
static_assert(std::is_trivially_copyable_v<Candle>);

// The 4h bucket `open_time_ms` falls into. Integer division, so this is the
// HTF bar's own open_time once the group completes.
constexpr int64_t HtfBucketStart(int64_t open_time_ms) noexcept {
    return (open_time_ms / kHtfMs) * kHtfMs;
}

}  // namespace screener
