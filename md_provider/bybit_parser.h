#pragma once

// BybitParser: everything that knows Bybit's wire format.
//
// This is the boundary. Bybit's `confirm` flag, its `start`/`ts` split, its
// uppercase symbols, its array-of-arrays REST kline rows and its newest-first
// ordering all stop here. md_core only ever sees a normalised Candle.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "md_provider/base_parser.h"
#include "types/candle.h"

namespace screener {

// Transparent hash, so a lookup by std::string_view does NOT construct a
// std::string. That is what keeps the symbol->id mapping off the message path
// in the sense that matters: the wire carries a symbol string and something
// must map it to an id exactly once, at this boundary, without allocating.
struct StringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view sv) const noexcept { return std::hash<std::string_view>{}(sv); }
};

using SymbolIdMap = std::unordered_map<std::string, uint32_t, StringHash, std::equal_to<>>;

class BybitParser : public Parser {
   public:
    enum class FrameKind {
        kKline,
        kPong,
        kSubscribeAck,
        kUnknown,
    };

    // Cheap raw scan, no JSON parse. Decides which handler a frame goes to.
    //
    // This is a ROUTER, not a filter: it never decides whether a bar is
    // confirmed. `data` is an array and each element carries its own
    // `confirm`, so that check belongs in the parse, per element.
    static FrameKind ClassifyFrame(std::string_view raw) noexcept;

    // Appends one Candle per CONFIRMED bar in the frame, with symbol_id
    // already resolved. Returns false only when the frame is malformed; a
    // frame for a symbol we do not track is not an error.
    bool ParseKlineFrame(std::string_view raw, const SymbolIdMap& ids, std::vector<Candle>& out);

    // /v5/market/kline. Appends candles OLDEST-FIRST.
    //
    // Bybit returns `result.list` NEWEST-FIRST, so this reverses it. Feeding
    // the rows as received runs the recursive indicators backwards and
    // produces values that look plausible and are wrong - which is why this
    // reversal lives here, once, rather than at each call site.
    bool ParseRestKline(std::string_view body, uint32_t symbol_id, std::vector<Candle>& out);

    // /v5/market/instruments-info. Appends the symbols of every
    // LinearPerpetual with status "Trading" and quoteCoin "USDT".
    //
    // `next_cursor` is set to result.nextPageCursor and is EMPTY on the last
    // page. Pagination is mandatory: the endpoint defaults to 500 entries and
    // there are more than 500 linear symbols, so a single default request
    // silently truncates the universe.
    bool ParseInstruments(std::string_view body, std::vector<std::string>& symbols, std::string& next_cursor);
};

}  // namespace screener
