#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "md_provider/bybit_parser.h"

using namespace screener;

namespace {

SymbolIdMap Ids() {
    SymbolIdMap ids;
    ids.emplace("BTCUSDT", 0u);
    ids.emplace("ETHUSDT", 1u);
    return ids;
}

// A real-shaped Bybit v5 linear kline frame. `start`/`end`/`timestamp` are
// NUMBERS and the prices are STRINGS - which is the opposite of the REST
// shape, and exactly the kind of thing that belongs pinned in a test.
std::string KlineFrame(const char* symbol, int64_t start, bool confirm, const char* close, const char* turnover) {
    return std::string(R"({"topic":"kline.60.)") + symbol + R"(","data":[{"start":)" + std::to_string(start) +
           R"(,"end":)" + std::to_string(start + 3'599'999) + R"(,"interval":"60","open":"100.5","close":")" + close +
           R"(","high":"110.25","low":"99.5","volume":"123.45","turnover":")" + turnover + R"(","confirm":)" +
           (confirm ? "true" : "false") + R"(,"timestamp":)" + std::to_string(start + 1000) + R"(}],"ts":)" +
           std::to_string(start + 3'600'000) + R"(,"type":"snapshot"})";
}

}  // namespace

// ---------------------------------------------------------------------------
// Frame routing
// ---------------------------------------------------------------------------

TEST(BybitParser, ClassifiesFrames) {
    EXPECT_EQ(BybitParser::ClassifyFrame(R"({"success":true,"ret_msg":"pong","op":"ping"})"),
              BybitParser::FrameKind::kPong);
    EXPECT_EQ(BybitParser::ClassifyFrame(R"({"op":"pong","args":["1700000000000"]})"), BybitParser::FrameKind::kPong);
    EXPECT_EQ(BybitParser::ClassifyFrame(R"({"success":true,"op":"subscribe","conn_id":"x"})"),
              BybitParser::FrameKind::kSubscribeAck);
    EXPECT_EQ(BybitParser::ClassifyFrame(KlineFrame("BTCUSDT", 0, true, "100", "1")), BybitParser::FrameKind::kKline);
    EXPECT_EQ(BybitParser::ClassifyFrame(R"({"something":"else"})"), BybitParser::FrameKind::kUnknown);
}

// ---------------------------------------------------------------------------
// WS kline
// ---------------------------------------------------------------------------

TEST(BybitParser, ParsesAConfirmedBar) {
    BybitParser parser;
    const auto ids = Ids();
    std::vector<Candle> out;

    const int64_t start = 1'700'000'000'000;
    ASSERT_TRUE(parser.ParseKlineFrame(KlineFrame("BTCUSDT", start, true, "105.75", "2500000.5"), ids, out));

    ASSERT_EQ(out.size(), 1u);
    const Candle& c = out[0];
    EXPECT_EQ(c.symbol_id, 0u);
    EXPECT_EQ(c.open_time_ms, start);               // `start`, not `timestamp`
    EXPECT_EQ(c.event_time_ms, start + 3'600'000);  // the envelope `ts`
    EXPECT_EQ(c.open, static_cast<Price>(100.5 * static_cast<double>(kPriceScale)));
    EXPECT_EQ(c.high, static_cast<Price>(110.25 * static_cast<double>(kPriceScale)));
    EXPECT_EQ(c.low, static_cast<Price>(99.5 * static_cast<double>(kPriceScale)));
    EXPECT_EQ(c.close, static_cast<Price>(105.75 * static_cast<double>(kPriceScale)));
}

// Act on closed bars only. An unconfirmed bar is the in-progress hour,
// republished every few hundred milliseconds; feeding it to a recursive
// indicator would apply a value that is about to change.
TEST(BybitParser, SkipsUnconfirmedBars) {
    BybitParser parser;
    const auto ids = Ids();
    std::vector<Candle> out;

    ASSERT_TRUE(parser.ParseKlineFrame(KlineFrame("BTCUSDT", 1'700'000'000'000, false, "105", "1"), ids, out));
    EXPECT_TRUE(out.empty());
}

// `data` is an ARRAY, so `confirm` must be checked PER ELEMENT. A cheap raw
// scan for "confirm":true can only ever be a prefilter, never the decision.
TEST(BybitParser, ChecksConfirmPerElementNotPerFrame) {
    BybitParser parser;
    const auto ids = Ids();
    std::vector<Candle> out;

    const std::string frame =
        R"({"topic":"kline.60.BTCUSDT","data":[)"
        R"({"start":0,"interval":"60","open":"1","close":"1","high":"1","low":"1","volume":"1","turnover":"1","confirm":true,"timestamp":0},)"
        R"({"start":3600000,"interval":"60","open":"2","close":"2","high":"2","low":"2","volume":"2","turnover":"2","confirm":false,"timestamp":0}],)"
        R"("ts":7200000,"type":"snapshot"})";

    ASSERT_TRUE(parser.ParseKlineFrame(frame, ids, out));
    ASSERT_EQ(out.size(), 1u) << "only the confirmed element may be emitted";
    EXPECT_EQ(out[0].open_time_ms, 0);
}

TEST(BybitParser, UsesTurnoverNotVolume) {
    BybitParser parser;
    const auto ids = Ids();
    std::vector<Candle> out;

    // volume is "123.45", turnover is "999.5" in the fixture below.
    ASSERT_TRUE(parser.ParseKlineFrame(KlineFrame("BTCUSDT", 0, true, "100", "999.5"), ids, out));
    ASSERT_EQ(out.size(), 1u);
    // turnover is quote volume (USDT) and is comparable across symbols; base
    // volume is denominated in a different coin per symbol and is not.
    EXPECT_EQ(out[0].turnover, static_cast<Volume>(999.5 * static_cast<double>(kVolumeScale)));
}

TEST(BybitParser, IgnoresAnUntrackedSymbolWithoutError) {
    BybitParser parser;
    const auto ids = Ids();
    std::vector<Candle> out;

    EXPECT_TRUE(parser.ParseKlineFrame(KlineFrame("DOGEUSDT", 0, true, "1", "1"), ids, out));
    EXPECT_TRUE(out.empty());
}

TEST(BybitParser, ResolvesTheSymbolIdFromTheTopic) {
    BybitParser parser;
    const auto ids = Ids();
    std::vector<Candle> out;

    ASSERT_TRUE(parser.ParseKlineFrame(KlineFrame("ETHUSDT", 0, true, "1", "1"), ids, out));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].symbol_id, 1u);
}

TEST(BybitParser, RejectsMalformedJson) {
    BybitParser parser;
    const auto ids = Ids();
    std::vector<Candle> out;
    EXPECT_FALSE(parser.ParseKlineFrame(R"({"topic":"kline.60.BTCUSDT","data":)", ids, out));
}

TEST(BybitParser, ParsesMicroPricesWithoutLosingDigits) {
    BybitParser parser;
    const auto ids = Ids();
    std::vector<Candle> out;

    const std::string frame = R"({"topic":"kline.60.BTCUSDT","data":[{"start":0,"interval":"60",)"
                              R"("open":"0.0000061","close":"0.0000062","high":"0.0000063","low":"0.0000060",)"
                              R"("volume":"1","turnover":"1","confirm":true,"timestamp":0}],"ts":1,"type":"snapshot"})";

    ASSERT_TRUE(parser.ParseKlineFrame(frame, ids, out));
    ASSERT_EQ(out.size(), 1u);
    // At kPriceScale = 1e10 these are exact integers: 61000, 62000, ...
    EXPECT_EQ(out[0].open, 61'000);
    EXPECT_EQ(out[0].close, 62'000);
    EXPECT_EQ(out[0].high, 63'000);
    EXPECT_EQ(out[0].low, 60'000);
}

// ---------------------------------------------------------------------------
// REST kline
// ---------------------------------------------------------------------------

// THE REVERSAL. Bybit returns result.list NEWEST-FIRST. Applying the rows as
// received runs the recursive indicators backwards and produces values that
// look plausible and are wrong, so the reversal lives in the parser, once.
TEST(BybitParser, RestKlineComesOutOldestFirst) {
    BybitParser parser;
    std::vector<Candle> out;

    // Newest first, as the venue sends it.
    const std::string body = R"({"retCode":0,"retMsg":"OK","result":{"symbol":"BTCUSDT","category":"linear","list":[)"
                             R"(["7200000","30","31","29","30.5","1","300"],)"
                             R"(["3600000","20","21","19","20.5","1","200"],)"
                             R"(["0","10","11","9","10.5","1","100"]]}})";

    ASSERT_TRUE(parser.ParseRestKline(body, 5, out));
    ASSERT_EQ(out.size(), 3u);

    EXPECT_EQ(out[0].open_time_ms, 0);
    EXPECT_EQ(out[1].open_time_ms, 3'600'000);
    EXPECT_EQ(out[2].open_time_ms, 7'200'000);

    EXPECT_EQ(out[0].symbol_id, 5u);
    EXPECT_EQ(out[0].open, static_cast<Price>(10.0 * static_cast<double>(kPriceScale)));
    EXPECT_EQ(out[0].high, static_cast<Price>(11.0 * static_cast<double>(kPriceScale)));
    EXPECT_EQ(out[0].low, static_cast<Price>(9.0 * static_cast<double>(kPriceScale)));
    EXPECT_EQ(out[0].close, static_cast<Price>(10.5 * static_cast<double>(kPriceScale)));
    EXPECT_EQ(out[0].turnover, static_cast<Volume>(100.0 * static_cast<double>(kVolumeScale)));
}

// The reversal must apply only to the rows this call appended, or a second
// fetch would scramble the first one's bars.
TEST(BybitParser, RestKlineReverseOnlyTouchesItsOwnAppend) {
    BybitParser parser;
    std::vector<Candle> out;

    Candle sentinel{};
    sentinel.open_time_ms = -1;
    out.push_back(sentinel);

    const std::string body =
        R"({"retCode":0,"result":{"list":[["3600000","2","2","2","2","1","2"],["0","1","1","1","1","1","1"]]}})";

    ASSERT_TRUE(parser.ParseRestKline(body, 0, out));
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].open_time_ms, -1) << "the pre-existing element must not move";
    EXPECT_EQ(out[1].open_time_ms, 0);
    EXPECT_EQ(out[2].open_time_ms, 3'600'000);
}

TEST(BybitParser, RestKlineRejectsNonZeroRetCode) {
    BybitParser parser;
    std::vector<Candle> out;
    EXPECT_FALSE(parser.ParseRestKline(R"({"retCode":10006,"retMsg":"too frequent","result":{}})", 0, out));
}

// ---------------------------------------------------------------------------
// instruments-info
// ---------------------------------------------------------------------------

TEST(BybitParser, InstrumentsKeepsOnlyTradingLinearPerpetualUsdt) {
    BybitParser parser;
    std::vector<std::string> symbols;
    std::string cursor;

    const std::string body =
        R"({"retCode":0,"result":{"list":[)"
        R"({"symbol":"BTCUSDT","contractType":"LinearPerpetual","status":"Trading","quoteCoin":"USDT"},)"
        R"({"symbol":"ETHPERP","contractType":"LinearPerpetual","status":"Trading","quoteCoin":"USDC"},)"
        R"({"symbol":"BTC-29DEC23","contractType":"LinearFutures","status":"Trading","quoteCoin":"USDT"},)"
        R"({"symbol":"DEADUSDT","contractType":"LinearPerpetual","status":"Closed","quoteCoin":"USDT"},)"
        R"({"symbol":"ETHUSDT","contractType":"LinearPerpetual","status":"Trading","quoteCoin":"USDT"}],)"
        R"("nextPageCursor":""}})";

    ASSERT_TRUE(parser.ParseInstruments(body, symbols, cursor));

    // USDC pair: turnover not comparable with the USDT universe.
    // Dated future: its last bars before expiry look like a volume collapse.
    // Closed: not tradable.
    ASSERT_EQ(symbols.size(), 2u);
    EXPECT_EQ(symbols[0], "BTCUSDT");
    EXPECT_EQ(symbols[1], "ETHUSDT");
    EXPECT_TRUE(cursor.empty());
}

// Pagination is mandatory: the endpoint defaults to 500 entries and there are
// more than 500 linear symbols, so one default request silently truncates the
// universe - and a truncated universe is not an error anyone would notice.
TEST(BybitParser, InstrumentsReportsTheNextCursor) {
    BybitParser parser;
    std::vector<std::string> symbols;
    std::string cursor;

    const std::string body =
        R"({"retCode":0,"result":{"list":[)"
        R"({"symbol":"BTCUSDT","contractType":"LinearPerpetual","status":"Trading","quoteCoin":"USDT"}],)"
        R"("nextPageCursor":"page2token"}})";

    ASSERT_TRUE(parser.ParseInstruments(body, symbols, cursor));
    EXPECT_EQ(cursor, "page2token");
}

TEST(BybitParser, InstrumentsAccumulatesAcrossPages) {
    BybitParser parser;
    std::vector<std::string> symbols;
    std::string cursor;

    const std::string page1 =
        R"({"retCode":0,"result":{"list":[{"symbol":"AUSDT","contractType":"LinearPerpetual","status":"Trading","quoteCoin":"USDT"}],"nextPageCursor":"c"}})";
    const std::string page2 =
        R"({"retCode":0,"result":{"list":[{"symbol":"BUSDT","contractType":"LinearPerpetual","status":"Trading","quoteCoin":"USDT"}],"nextPageCursor":""}})";

    ASSERT_TRUE(parser.ParseInstruments(page1, symbols, cursor));
    ASSERT_TRUE(parser.ParseInstruments(page2, symbols, cursor));

    ASSERT_EQ(symbols.size(), 2u);
    EXPECT_EQ(symbols[0], "AUSDT");
    EXPECT_EQ(symbols[1], "BUSDT");
    EXPECT_TRUE(cursor.empty());
}
