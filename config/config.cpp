#include "config/config.h"

#include <charconv>
#include <string_view>

namespace screener {
namespace {

// Returns false on a malformed number rather than clamping or defaulting.
// A typo in --warmup-bars should stop the run, not quietly change the
// indicator warm-up window.
bool ParseUint(std::string_view value, uint32_t& out) {
    if (value.empty()) {
        return false;
    }
    const auto result = std::from_chars(value.data(), value.data() + value.size(), out);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size();
}

bool Match(std::string_view arg, std::string_view flag, std::string_view& value) {
    if (arg.size() <= flag.size() || arg.compare(0, flag.size(), flag) != 0) {
        return false;
    }
    value = arg.substr(flag.size());
    return true;
}

}  // namespace

std::optional<ScreenerConfig> ScreenerConfig::FromArgs(int argc, char* argv[]) {
    ScreenerConfig config;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        std::string_view value;

        if (arg == "--verbose") {
            config.verbose = true;
        } else if (Match(arg, "--max-symbols=", value)) {
            if (!ParseUint(value, config.max_symbols)) {
                return std::nullopt;
            }
        } else if (Match(arg, "--warmup-bars=", value)) {
            if (!ParseUint(value, config.warmup_ltf_bars)) {
                return std::nullopt;
            }
        } else if (Match(arg, "--topics-per-sub=", value)) {
            if (!ParseUint(value, config.topics_per_subscribe)) {
                return std::nullopt;
            }
        } else {
            // Unknown flags are rejected, not ignored. A silently dropped
            // --warmup-bar=603 (singular) would look like it worked.
            return std::nullopt;
        }
    }

    if (!config.Validate().empty()) {
        return std::nullopt;
    }
    return config;
}

std::string ScreenerConfig::Validate() const {
    if (warmup_ltf_bars < 4) {
        return "warmup_ltf_bars must be at least 4 (one 4h group)";
    }
    // Bybit's kline endpoint caps limit at 1000. Asking for more would not
    // error - it would silently return 1000, and the warm-up would be shorter
    // than the EMA50 needs without anything saying so.
    if (warmup_ltf_bars > 1000) {
        return "warmup_ltf_bars must be <= 1000 (Bybit kline limit)";
    }
    if (topics_per_subscribe == 0) {
        return "topics_per_subscribe must be > 0";
    }
    if (ping_interval_s == 0 || ping_interval_s > 20) {
        return "ping_interval_s must be in 1..20 (Bybit closes an idle socket at 20s)";
    }
    if (max_missed_pongs == 0) {
        return "max_missed_pongs must be > 0";
    }
    return {};
}

}  // namespace screener
