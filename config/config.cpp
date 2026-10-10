#include "config/config.h"

#include <sys/stat.h>

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <simdjson.h>
#include <sstream>
#include <string>
#include <string_view>

#include "md_provider/decimal.h"

namespace screener {
namespace {

namespace ondemand = simdjson::ondemand;

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

std::string_view Trim(std::string_view in) {
    while (!in.empty() && (in.front() == ' ' || in.front() == '\t' || in.front() == '\r')) {
        in.remove_prefix(1);
    }
    while (!in.empty() && (in.back() == ' ' || in.back() == '\t' || in.back() == '\r')) {
        in.remove_suffix(1);
    }
    return in;
}

// Extracts the VALUE from everything right of the '=', which may carry an
// inline comment: `KEY=value  # why` or `KEY="value"  # why`.
//
// Both forms are written by hand constantly, and getting this wrong is not a
// parse error - it is a secret that silently gains a `# why` suffix and
// produces an unexplainable 400 from the far end hours later. The quoted form
// has to be handled separately because the comment puts a non-quote character
// last, so "strip matching outer quotes" never fires.
//
// Nothing inside the quotes is unescaped: this is not a shell.
//
// Returns false when a quoted value has no closing quote, or when a quoted
// value is followed by something that is not a comment.
bool ExtractValue(std::string_view in, std::string_view& out) {
    if (!in.empty() && (in.front() == '"' || in.front() == '\'')) {
        const char quote = in.front();
        const std::size_t close = in.find(quote, 1);
        if (close == std::string_view::npos) {
            return false;
        }
        const std::string_view rest = Trim(in.substr(close + 1));
        if (!rest.empty() && rest.front() != '#') {
            return false;
        }
        out = in.substr(1, close - 1);
        return true;
    }

    // Unquoted: an inline comment starts at the first '#'. A '#' that belongs
    // inside the value must therefore be quoted - which is the same rule every
    // other .env reader uses, so a file that works here works there.
    const std::size_t hash = in.find('#');
    out = Trim(hash == std::string_view::npos ? in : in.substr(0, hash));
    return true;
}

// nullptr when every character may appear unescaped in a URL path or query,
// otherwise the first that may not. Control characters, space, quotes and '#'
// are the ones a hand-written .env actually produces.
const char* FirstUrlUnsafe(std::string_view in) {
    for (const char& ch : in) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte <= ' ' || byte == 0x7F || ch == '"' || ch == '\'' || ch == '#') {
            return &ch;
        }
    }
    return nullptr;
}

// nullopt if the file cannot be opened; its whole content otherwise.
std::optional<std::string> ReadFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// A secrets file other users on the box can read defeats the point of keeping
// the token out of argv. Warned about, not rejected: a 644 .env on a dev
// laptop is normal, and refusing to start over it would be the wrong trade.
void WarnIfWorldReadable(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        return;
    }
    if ((st.st_mode & (S_IRGRP | S_IROTH)) != 0) {
        std::fprintf(stderr, "screener: %s is readable by other users (mode %o) - chmod 600 it\n", path.c_str(),
                     static_cast<unsigned>(st.st_mode & 07777));
    }
}

}  // namespace

std::string ApplyDotEnv(std::string_view content, ScreenerConfig& config) {
    int line_no = 0;

    while (!content.empty()) {
        const std::size_t end = content.find('\n');
        std::string_view line = content.substr(0, end);
        content = end == std::string_view::npos ? std::string_view{} : content.substr(end + 1);
        ++line_no;

        line = Trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }

        // `export KEY=VALUE` is accepted because that is what a line copied
        // out of a shell looks like.
        constexpr std::string_view kExport = "export ";
        if (line.size() > kExport.size() && line.compare(0, kExport.size(), kExport) == 0) {
            line = Trim(line.substr(kExport.size()));
        }

        const std::string where = config.env_file + ": line " + std::to_string(line_no) + ": ";

        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            return where + "expected KEY=VALUE";
        }

        const std::string_view key = Trim(line.substr(0, eq));

        std::string_view value;
        if (!ExtractValue(Trim(line.substr(eq + 1)), value)) {
            return where + "unterminated or malformed quoted value";
        }

        if (key.empty()) {
            return where + "empty key";
        }

        if (key == "TELEGRAM_BOT_TOKEN" || key == "TELEGRAM_CHAT_ID") {
            // An empty value for a key we care about is a mistake, not a
            // default: the operator clearly meant to set it, and leaving it
            // empty would surface as "--telegram needs TELEGRAM_BOT_TOKEN"
            // with the variable sitting right there in the file.
            if (value.empty()) {
                return where + std::string(key) + " is empty";
            }
        }

        if (key == "TELEGRAM_BOT_TOKEN") {
            config.telegram_token = value;
        } else if (key == "TELEGRAM_CHAT_ID") {
            config.telegram_chat_id = value;
        }
        // Any other key belongs to another tool sharing this file. Ignored.
    }

    return {};
}

std::string ApplyConfigJson(std::string_view content, ScreenerConfig& config) {
    ondemand::parser parser;
    simdjson::padded_string padded(content);  // startup-only: not the message path

    ondemand::document doc;
    if (parser.iterate(padded).get(doc)) {
        return config.config_file + ": not valid JSON";
    }

    ondemand::object obj;
    if (doc.get_object().get(obj)) {
        return config.config_file + ": expected a JSON object";
    }

    for (auto field : obj) {
        std::string_view key;
        if (field.unescaped_key().get(key)) {
            return config.config_file + ": malformed key";
        }
        // A "//"-prefixed key is a COMMENT, not a typo. Plain JSON has no
        // comment syntax, and every other key is fatal if unrecognised - so
        // without an escape, example_config.json could not annotate a field
        // without becoming unusable as a direct copy to config.json.
        if (key.size() >= 2 && key.substr(0, 2) == "//") {
            continue;
        }

        const std::string where = config.config_file + ": \"" + std::string(key) + "\": ";

        if (key == "ema_period") {
            uint64_t v = 0;
            if (field.value().get_uint64().get(v) || v <= 1 || v > 1'000'000) {
                return where + "expected an integer > 1";
            }
            config.ema_period = static_cast<uint32_t>(v);
        } else if (key == "surge_numerator") {
            int64_t v = 0;
            if (field.value().get_int64().get(v) || v <= 0) {
                return where + "expected an integer > 0";
            }
            config.surge_numerator = v;
        } else if (key == "surge_denominator") {
            int64_t v = 0;
            if (field.value().get_int64().get(v) || v <= 0) {
                return where + "expected an integer > 0";
            }
            config.surge_denominator = v;
        } else if (key == "min_natr_bp") {
            int64_t v = 0;
            if (field.value().get_int64().get(v) || v < 0 || v > 1'000'000) {
                return where + "expected an integer >= 0";
            }
            config.min_natr_bp = static_cast<int32_t>(v);
        } else if (key == "min_turnover_usdt") {
            // A STRING, not a JSON number - a fractional USDT amount parsed
            // through a double intermediate would reintroduce exactly the
            // rounding error the scaled-integer representation exists to
            // avoid. Same rule as every venue-reported price/volume.
            std::string_view v;
            if (field.value().get_string().get(v) || v.empty()) {
                return where + "expected a decimal string, e.g. \"1000000.00\"";
            }
            config.min_turnover = static_cast<Volume>(ParseScaledDecimal<6>(v));
        } else {
            // Unlike .env, nothing else shares this file - an unrecognised
            // key is a typo the operator should hear about, not a silently
            // ignored line.
            return where + "unrecognised key";
        }
    }

    return {};
}

std::optional<ScreenerConfig> ScreenerConfig::FromArgs(int argc, char* argv[]) {
    ScreenerConfig config;

    // Set by --env-file=. It decides whether a missing file is an error: the
    // default .env is optional, a path the operator typed is not.
    bool env_file_explicit = false;

    // Same policy, for --config= / config.json.
    bool config_file_explicit = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        std::string_view value;

        if (arg == "--verbose") {
            config.verbose = true;
        } else if (arg == "--telegram") {
            config.telegram_enabled = true;
        } else if (Match(arg, "--env-file=", value)) {
            config.env_file = std::string(value);
            env_file_explicit = true;
        } else if (Match(arg, "--config=", value)) {
            config.config_file = std::string(value);
            config_file_explicit = true;
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

    // Secrets, AFTER the flag loop because --env-file= decides where to look.
    //
    // Two sources, file first and then the environment, because THE SHELL
    // WINS: the file supplies defaults so a normal run needs no exports, and
    // an explicit export still points one run at a test chat without editing
    // the file.
    //
    // Neither source ENABLES notifications - --telegram does. An exported
    // token, or one left in a .env, must not make a run start talking to a
    // chat the operator did not ask for.
    if (const std::optional<std::string> content = ReadFile(config.env_file)) {
        WarnIfWorldReadable(config.env_file);
        if (const std::string error = ApplyDotEnv(*content, config); !error.empty()) {
            std::fprintf(stderr, "screener: %s\n", error.c_str());
            return std::nullopt;
        }
    } else if (env_file_explicit) {
        // Only an error when the path was typed. A missing default .env means
        // "use the environment", which is the CI and container case.
        std::fprintf(stderr, "screener: --env-file=%s cannot be read\n", config.env_file.c_str());
        return std::nullopt;
    }

    // config.json: optional, same missing-vs-explicit policy as .env. Loaded
    // after flags (so --config= decides the path) and before Validate() (so
    // a bad threshold is caught there, in one place, rather than twice).
    if (const std::optional<std::string> content = ReadFile(config.config_file)) {
        if (const std::string error = ApplyConfigJson(*content, config); !error.empty()) {
            std::fprintf(stderr, "screener: %s\n", error.c_str());
            return std::nullopt;
        }
    } else if (config_file_explicit) {
        std::fprintf(stderr, "screener: --config=%s cannot be read\n", config.config_file.c_str());
        return std::nullopt;
    }

    if (const char* token = std::getenv("TELEGRAM_BOT_TOKEN")) {
        config.telegram_token = token;
    }
    if (const char* chat_id = std::getenv("TELEGRAM_CHAT_ID")) {
        config.telegram_chat_id = chat_id;
    }

    // Print the REASON before returning nullopt. The caller only knows
    // "unusable" and answers with the usage text, so without this line
    // "--telegram with no TELEGRAM_BOT_TOKEN" and "--warmup-bars=99999" are
    // both reported as a syntax error, which is neither of them.
    if (const std::string error = config.Validate(); !error.empty()) {
        std::fprintf(stderr, "screener: %s\n", error.c_str());
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
    if (surge_numerator <= surge_denominator) {
        return "surge_numerator must be > surge_denominator (a surge ratio <= 1 would always pass)";
    }
    if (min_turnover < 0) {
        return "min_turnover_usdt must be >= 0";
    }
    if (telegram_enabled) {
        // Refuse to start rather than run with notifications silently off.
        // --telegram is an explicit request; honouring it as a no-op because
        // an env var is missing is exactly the failure the operator would not
        // notice until the first ACTIVE never arrived.
        if (telegram_token.empty()) {
            return "--telegram needs TELEGRAM_BOT_TOKEN in .env or the environment";
        }
        if (telegram_chat_id.empty()) {
            return "--telegram needs TELEGRAM_CHAT_ID in .env or the environment";
        }
        // Both go into the request LINE - the token in the path, the chat_id
        // in the query - so a stray space, quote or '#' produces a malformed
        // HTTP request and a 400 from the server's parser, long before the Bot
        // API sees it. Caught here, where the message can name the cause,
        // rather than as an unexplainable status code an hour into a run.
        if (const char* bad = FirstUrlUnsafe(telegram_token)) {
            return std::string("TELEGRAM_BOT_TOKEN contains an illegal character ('") + *bad +
                   "') - check for quotes or a trailing comment in .env";
        }
        if (const char* bad = FirstUrlUnsafe(telegram_chat_id)) {
            return std::string("TELEGRAM_CHAT_ID contains an illegal character ('") + *bad +
                   "') - check for quotes or a trailing comment in .env";
        }
        if (telegram_min_send_interval_ms == 0) {
            return "telegram_min_send_interval_ms must be > 0 (Telegram rate-limits per chat)";
        }
        if (telegram_max_queue == 0) {
            return "telegram_max_queue must be > 0";
        }
    }
    return {};
}

}  // namespace screener
