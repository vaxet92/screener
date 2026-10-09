#include "md_provider/bybit_parser.h"

#include <algorithm>
#include <charconv>

#include "logger/logger.h"
#include "md_provider/decimal.h"

namespace screener {
namespace {

namespace ondemand = simdjson::ondemand;

// Prices at kPriceScale (1e10), turnover at kVolumeScale (1e6). Parsed
// straight from the venue's decimal string into a scaled integer - never via
// a double, which would reintroduce exactly the rounding error the scaled
// representation exists to avoid.
Price ParsePrice(std::string_view sv) noexcept {
    return static_cast<Price>(ParseScaledDecimal<10>(sv));
}

Volume ParseTurnover(std::string_view sv) noexcept {
    return static_cast<Volume>(ParseScaledDecimal<6>(sv));
}

bool ParseInt64(std::string_view sv, int64_t& out) noexcept {
    const auto r = std::from_chars(sv.data(), sv.data() + sv.size(), out);
    return r.ec == std::errc{} && r.ptr == sv.data() + sv.size();
}

}  // namespace

BybitParser::FrameKind BybitParser::ClassifyFrame(std::string_view raw) noexcept {
    // Bybit answers our {"op":"ping"} with a frame carrying "pong" - in
    // `ret_msg` on the public linear stream, in `op` on some endpoints.
    // Matching the substring covers both without caring which.
    if (raw.find("pong") != std::string_view::npos) {
        return FrameKind::kPong;
    } else if (raw.find("\"topic\":\"kline.") != std::string_view::npos) {
        return FrameKind::kKline;
    } else if (raw.find("\"op\":\"subscribe\"") != std::string_view::npos) {
        return FrameKind::kSubscribeAck;
    }
    return FrameKind::kUnknown;
}

bool BybitParser::ParseKlineFrame(std::string_view raw, const SymbolIdMap& ids, std::vector<Candle>& out) {
    ondemand::document doc;
    if (parser_.iterate(Load(raw)).get(doc)) {
        return false;
    }

    std::string_view topic;
    if (doc["topic"].get_string().get(topic)) {
        return false;
    }

    // "kline.60.BTCUSDT" -> "BTCUSDT"
    const auto dot = topic.rfind('.');
    if (dot == std::string_view::npos) {
        return false;
    }
    const std::string_view symbol = topic.substr(dot + 1);

    const auto it = ids.find(symbol);
    if (it == ids.end()) {
        return true;  // a topic we did not subscribe to: ignore, not an error
    }
    const uint32_t symbol_id = it->second;

    // Envelope `ts`: when the VENUE SENT this message. It describes the
    // message, never the bar, and is a latency input only - open_time is the
    // bar's sole identity.
    int64_t event_time_ms = 0;
    if (doc["ts"].get_int64().get(event_time_ms)) {
        event_time_ms = 0;  // tolerate: the bar is still usable without it
    }

    ondemand::array data;
    if (doc["data"].get_array().get(data)) {
        return false;
    }

    for (auto element : data) {
        ondemand::object bar;
        if (element.get_object().get(bar)) {
            return false;
        }

        bool confirm = false;
        if (bar["confirm"].get_bool().get(confirm)) {
            continue;
        }
        // Act on closed bars only. An unconfirmed bar is the in-progress
        // hour, republished every few hundred ms; applying it would feed the
        // recursive indicators a value that is about to change.
        if (!confirm) {
            continue;
        }

        Candle c{};
        c.symbol_id = symbol_id;
        c.event_time_ms = event_time_ms;

        if (bar["start"].get_int64().get(c.open_time_ms)) {
            return false;
        }

        std::string_view sv;
        if (bar["open"].get_string().get(sv)) {
            return false;
        }
        c.open = ParsePrice(sv);
        if (bar["high"].get_string().get(sv)) {
            return false;
        }
        c.high = ParsePrice(sv);
        if (bar["low"].get_string().get(sv)) {
            return false;
        }
        c.low = ParsePrice(sv);
        if (bar["close"].get_string().get(sv)) {
            return false;
        }
        c.close = ParsePrice(sv);
        // `turnover` (quote volume, USDT), NOT `volume` (base volume). The
        // surge condition compares symbols against each other, and base
        // volume is denominated in a different coin per symbol, so it is not
        // comparable across the universe.
        if (bar["turnover"].get_string().get(sv)) {
            return false;
        }
        c.turnover = ParseTurnover(sv);

        out.push_back(c);
    }

    return true;
}

bool BybitParser::ParseRestKline(std::string_view body, uint32_t symbol_id, std::vector<Candle>& out) {
    ondemand::document doc;
    if (parser_.iterate(Load(body)).get(doc)) {
        return false;
    }

    int64_t ret_code = -1;
    if (doc["retCode"].get_int64().get(ret_code) || ret_code != 0) {
        return false;
    }

    ondemand::object result;
    if (doc["result"].get_object().get(result)) {
        return false;
    }
    ondemand::array list;
    if (result["list"].get_array().get(list)) {
        return false;
    }

    const std::size_t first = out.size();

    for (auto row : list) {
        ondemand::array cols;
        if (row.get_array().get(cols)) {
            return false;
        }

        // Row layout, all STRINGS (unlike the WS payload, where the times are
        // numbers): [startTime, open, high, low, close, volume, turnover]
        std::string_view fields[7];
        int n = 0;
        for (auto col : cols) {
            if (n == 7) {
                break;
            }
            if (col.get_string().get(fields[n])) {
                return false;
            }
            ++n;
        }
        if (n < 7) {
            return false;
        }

        Candle c{};
        c.symbol_id = symbol_id;
        if (!ParseInt64(fields[0], c.open_time_ms)) {
            return false;
        }
        // A history bar has no message envelope. event_time is a property of
        // the message that delivered the bar, and a REST row was not pushed
        // to us - leaving it 0 is the honest value, not a defect.
        c.event_time_ms = 0;
        c.open = ParsePrice(fields[1]);
        c.high = ParsePrice(fields[2]);
        c.low = ParsePrice(fields[3]);
        c.close = ParsePrice(fields[4]);
        c.turnover = ParseTurnover(fields[6]);

        out.push_back(c);
    }

    // Bybit returns newest-first. The caller feeds these straight into the
    // recursive indicators, so they must come out oldest-first.
    std::reverse(out.begin() + static_cast<std::ptrdiff_t>(first), out.end());
    return true;
}

bool BybitParser::ParseInstruments(std::string_view body, std::vector<std::string>& symbols, std::string& next_cursor) {
    ondemand::document doc;
    if (parser_.iterate(Load(body)).get(doc)) {
        return false;
    }

    int64_t ret_code = -1;
    if (doc["retCode"].get_int64().get(ret_code) || ret_code != 0) {
        return false;
    }

    ondemand::object result;
    if (doc["result"].get_object().get(result)) {
        return false;
    }

    ondemand::array list;
    if (result["list"].get_array().get(list)) {
        return false;
    }

    for (auto entry : list) {
        ondemand::object o;
        if (entry.get_object().get(o)) {
            return false;
        }

        std::string_view symbol;
        if (o["symbol"].get_string().get(symbol)) {
            continue;
        }

        // Filter to USDT PERPETUALS that are actually tradable.
        //
        // `category=linear` also contains dated futures (LinearFutures) and
        // USDC pairs. A dated contract expires, and its last bars before
        // expiry look like a volume collapse; a USDC pair's turnover is not
        // comparable with the USDT universe. Both would be noise in the
        // screener's output rather than signal.
        std::string_view contract_type;
        if (o["contractType"].get_string().get(contract_type) || contract_type != "LinearPerpetual") {
            continue;
        }
        std::string_view status;
        if (o["status"].get_string().get(status) || status != "Trading") {
            continue;
        }
        std::string_view quote;
        if (o["quoteCoin"].get_string().get(quote) || quote != "USDT") {
            continue;
        }

        symbols.emplace_back(symbol);
    }

    // Empty on the last page - that is how the caller knows to stop, so a
    // missing field must read as "done", not as an error.
    std::string_view cursor;
    if (result["nextPageCursor"].get_string().get(cursor)) {
        next_cursor.clear();
    } else {
        next_cursor.assign(cursor);
    }

    return true;
}

}  // namespace screener
