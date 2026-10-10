// TelegramNotifier tests.
//
// No network and no bot token: every test injects a Sender that records the
// text, which is the whole reason the transport is a seam. What is worth
// testing here is NOT "does an HTTPS GET work" - it is the message format, the
// active-list cap, the pacing order and the drop policy, and all four are pure
// functions of the queue.

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <cstdio>
#include <boost/asio/ssl/context.hpp>
#include <chrono>
#include <string>
#include <unordered_set>
#include <vector>

#include "config/config.h"
#include "md_provider/ws/root_certificates.hpp"
#include "notifier/telegram_notifier.h"

namespace screener {
namespace {

// A configured-and-enabled bot with instant pacing. interval 1ms rather than 0
// keeps the timer path real - a 0ms timer still posts a completion handler, so
// the queue is still drained by the io_context and not by Enqueue.
ScreenerConfig EnabledConfig() {
    ScreenerConfig c;
    c.telegram_enabled = true;
    c.telegram_token = "123:FAKE";
    c.telegram_chat_id = "-1001";
    c.telegram_min_send_interval_ms = 1;
    c.telegram_max_queue = 100;
    // telegram_active_list_max is left at its default of 0 = list every
    // symbol; the one test that cares about truncation sets it itself.
    return c;
}

std::unordered_set<Symbol> ActiveSet(std::initializer_list<const char*> names) {
    std::unordered_set<Symbol> out;
    for (const char* n : names) {
        out.emplace(n);
    }
    return out;
}

// Runs the io_context until it has no work left, with a wall-clock bound so a
// stuck timer fails the test instead of hanging the suite.
void RunUntilIdle(boost::asio::io_context& ioc) {
    ioc.run_for(std::chrono::seconds(2));
}

}  // namespace

TEST(TelegramNotifier, DisabledWithoutTheFlag) {
    boost::asio::io_context ioc;
    std::vector<std::string> sent;

    ScreenerConfig c = EnabledConfig();
    c.telegram_enabled = false;

    TelegramNotifier n(ioc, c, [&sent](const std::string& t) { sent.push_back(t); });

    EXPECT_FALSE(n.Enabled());
    n.NotifyActivated("BTCUSDT", ActiveSet({"BTCUSDT"}));
    RunUntilIdle(ioc);

    // Not an error, not a queued message: a run without --telegram behaves
    // exactly as it did before this class existed.
    EXPECT_TRUE(sent.empty());
}

TEST(TelegramNotifier, DisabledWhenTheTokenIsMissing) {
    boost::asio::io_context ioc;
    ScreenerConfig c = EnabledConfig();
    c.telegram_token.clear();

    TelegramNotifier n(ioc, c, [](const std::string&) {});
    EXPECT_FALSE(n.Enabled());
}

TEST(TelegramNotifier, FirstMessageGoesOutWithoutWaiting) {
    boost::asio::io_context ioc;
    std::vector<std::string> sent;
    TelegramNotifier n(ioc, EnabledConfig(), [&sent](const std::string& t) { sent.push_back(t); });

    n.NotifyActivated("BTCUSDT", ActiveSet({"BTCUSDT", "ETHUSDT"}));

    // Before the io_context has run at all. The pacing timer delays the
    // messages BEHIND the first one; a lone transition must not wait an
    // interval for no reason.
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_NE(sent[0].find("<b>ACTIVE</b> <b>BTCUSDT</b>"), std::string::npos) << sent[0];
    EXPECT_NE(sent[0].find("~~~ACTIVE~~~ (2):"), std::string::npos) << sent[0];
    // One per line, each bold - this is read on a phone.
    EXPECT_NE(sent[0].find("\n<b>BTCUSDT</b>\n<b>ETHUSDT</b>\n"), std::string::npos) << sent[0];
}

TEST(TelegramNotifier, DeactivationNamesTheSymbolAndTheRemainingSet) {
    boost::asio::io_context ioc;
    std::vector<std::string> sent;
    TelegramNotifier n(ioc, EnabledConfig(), [&sent](const std::string& t) { sent.push_back(t); });

    n.NotifyDeactivated("SOLUSDT", ActiveSet({"BTCUSDT"}));
    RunUntilIdle(ioc);

    ASSERT_EQ(sent.size(), 1u);
    EXPECT_NE(sent[0].find("<b>INACTIVE</b> <b>SOLUSDT</b>"), std::string::npos) << sent[0];
    EXPECT_NE(sent[0].find("~~~ACTIVE~~~ (1):"), std::string::npos) << sent[0];
    EXPECT_NE(sent[0].find("<b>BTCUSDT</b>"), std::string::npos) << sent[0];
}

TEST(TelegramNotifier, StartupMessageExplainsTheReadyGap) {
    boost::asio::io_context ioc;
    std::vector<std::string> sent;
    TelegramNotifier n(ioc, EnabledConfig(), [&sent](const std::string& t) { sent.push_back(t); });

    TelegramNotifier::StartupReport report;
    report.tracked = 791;
    report.ready = 789;
    report.not_ready = {"AUSDT - no history (warm-up request failed)", "NEWUSDT - 12 x 1h, 3 x 4h (EMA50 needs 50)"};

    n.NotifyStarted(report, ActiveSet({}));
    RunUntilIdle(ioc);

    ASSERT_EQ(sent.size(), 1u);
    // The gap between tracked and ready is stated, not left as a subtraction,
    // and every missing symbol is named with its reason.
    EXPECT_NE(sent[0].find("791 instruments: 789 ready, 2 not ready"), std::string::npos) << sent[0];
    EXPECT_NE(sent[0].find("~~~NOT READY~~~ (2):"), std::string::npos) << sent[0];
    EXPECT_NE(sent[0].find("AUSDT - no history"), std::string::npos) << sent[0];
    EXPECT_NE(sent[0].find("NEWUSDT - 12 x 1h"), std::string::npos) << sent[0];
    EXPECT_NE(sent[0].find("~~~ACTIVE~~~ (0):"), std::string::npos) << sent[0];
}

TEST(TelegramNotifier, ListsEverySymbolByDefault) {
    boost::asio::io_context ioc;
    std::vector<std::string> sent;
    TelegramNotifier n(ioc, EnabledConfig(), [&sent](const std::string& t) { sent.push_back(t); });

    std::unordered_set<Symbol> active;
    for (int i = 0; i < 60; ++i) {
        active.emplace("SYM" + std::to_string(i) + "USDT");
    }

    n.NotifyActivated("SYM0USDT", active);
    RunUntilIdle(ioc);

    // telegram_active_list_max defaults to 0 = all. No "+N more" anywhere.
    std::string all;
    for (const std::string& part : sent) {
        all += part;
    }
    EXPECT_EQ(all.find("more"), std::string::npos) << all;
    for (const Symbol& symbol : active) {
        EXPECT_NE(all.find("<b>" + symbol + "</b>"), std::string::npos) << symbol;
    }
}

TEST(TelegramNotifier, SplitsAnOversizedListAcrossMessages) {
    boost::asio::io_context ioc;
    std::vector<std::string> sent;
    TelegramNotifier n(ioc, EnabledConfig(), [&sent](const std::string& t) { sent.push_back(t); });

    // ~1000 symbols at ~22 bytes a line is ~22KB - well past Telegram's 4096
    // character limit, which is the whole universe going ACTIVE.
    std::unordered_set<Symbol> active;
    for (int i = 0; i < 1000; ++i) {
        active.emplace("SYMBOL" + std::to_string(i) + "USDT");
    }

    n.NotifyActivated("SYMBOL0USDT", active);
    RunUntilIdle(ioc);

    ASSERT_GT(sent.size(), 1u);

    std::string all;
    for (const std::string& part : sent) {
        EXPECT_LE(part.size(), 4096u) << "a part would be rejected by Telegram";
        // A cut inside <b>...</b> is a 400 on both halves, so every part must
        // have balanced tags.
        std::size_t opens = 0;
        std::size_t closes = 0;
        for (std::size_t at = part.find("<b>"); at != std::string::npos; at = part.find("<b>", at + 1)) {
            ++opens;
        }
        for (std::size_t at = part.find("</b>"); at != std::string::npos; at = part.find("</b>", at + 1)) {
            ++closes;
        }
        EXPECT_EQ(opens, closes) << part.substr(0, 120);
        all += part;
    }

    // Nothing is lost in the split.
    for (const Symbol& symbol : active) {
        EXPECT_NE(all.find("<b>" + symbol + "</b>"), std::string::npos) << symbol;
    }
}

TEST(TelegramNotifier, EscapesHtmlInText) {
    boost::asio::io_context ioc;
    std::vector<std::string> sent;
    TelegramNotifier n(ioc, EnabledConfig(), [&sent](const std::string& t) { sent.push_back(t); });

    // Not a real ticker, but parse_mode=HTML means any unescaped '<' in a
    // dynamic value is a 400 - and the whole point of HTML over MarkdownV2 is
    // that there are only three characters to get right.
    n.NotifyActivated("A&B<C>", ActiveSet({}));
    RunUntilIdle(ioc);

    ASSERT_EQ(sent.size(), 1u);
    EXPECT_NE(sent[0].find("A&amp;B&lt;C&gt;"), std::string::npos) << sent[0];
}

TEST(TelegramNotifier, ActiveListIsCappedAndSorted) {
    boost::asio::io_context ioc;
    std::vector<std::string> sent;

    ScreenerConfig c = EnabledConfig();
    c.telegram_active_list_max = 3;
    TelegramNotifier n(ioc, c, [&sent](const std::string& t) { sent.push_back(t); });

    // Telegram rejects a message over 4096 characters, so the full set is
    // counted but not listed.
    n.NotifyActivated("AAAUSDT", ActiveSet({"DDDUSDT", "BBBUSDT", "AAAUSDT", "CCCUSDT", "EEEUSDT"}));
    RunUntilIdle(ioc);

    ASSERT_EQ(sent.size(), 1u);
    // Non-zero cap: the count is still the real one, and the remainder is
    // reported rather than silently missing.
    EXPECT_NE(sent[0].find("~~~ACTIVE~~~ (5):"), std::string::npos) << sent[0];
    EXPECT_NE(sent[0].find("<b>AAAUSDT</b>\n<b>BBBUSDT</b>\n<b>CCCUSDT</b>\n(+2 more)"), std::string::npos) << sent[0];
}

TEST(TelegramNotifier, PacesABurstInOrder) {
    boost::asio::io_context ioc;
    std::vector<std::string> sent;
    TelegramNotifier n(ioc, EnabledConfig(), [&sent](const std::string& t) { sent.push_back(t); });

    // The shape of a real hourly close: several symbols flip inside the same
    // handler, long before the io_context gets to run again.
    n.NotifyActivated("AAAUSDT", ActiveSet({"AAAUSDT"}));
    n.NotifyActivated("BBBUSDT", ActiveSet({"AAAUSDT", "BBBUSDT"}));
    n.NotifyActivated("CCCUSDT", ActiveSet({"AAAUSDT", "BBBUSDT", "CCCUSDT"}));

    EXPECT_EQ(sent.size(), 1u);  // one sent, two queued
    EXPECT_EQ(n.QueueDepth(), 2u);

    RunUntilIdle(ioc);

    ASSERT_EQ(sent.size(), 3u);
    EXPECT_NE(sent[0].find("AAAUSDT"), std::string::npos);
    EXPECT_NE(sent[1].find("BBBUSDT"), std::string::npos);
    EXPECT_NE(sent[2].find("CCCUSDT"), std::string::npos);
    EXPECT_EQ(n.Dropped(), 0u);
}

TEST(TelegramNotifier, DropsPastTheQueueCapAndReportsTheLoss) {
    boost::asio::io_context ioc;
    std::vector<std::string> sent;

    ScreenerConfig c = EnabledConfig();
    c.telegram_max_queue = 1;
    TelegramNotifier n(ioc, c, [&sent](const std::string& t) { sent.push_back(t); });

    n.NotifyActivated("AAAUSDT", ActiveSet({"AAAUSDT"}));  // sent immediately
    n.NotifyActivated("BBBUSDT", ActiveSet({"BBBUSDT"}));  // fills the queue
    n.NotifyActivated("CCCUSDT", ActiveSet({"CCCUSDT"}));  // dropped
    n.NotifyActivated("DDDUSDT", ActiveSet({"DDDUSDT"}));  // dropped

    EXPECT_EQ(n.Dropped(), 2u);

    RunUntilIdle(ioc);

    // The two that fit, in order, then one report of the loss - into the chat,
    // because the operator this class exists for is not reading the log.
    ASSERT_EQ(sent.size(), 3u);
    EXPECT_NE(sent[0].find("AAAUSDT"), std::string::npos);
    EXPECT_NE(sent[1].find("BBBUSDT"), std::string::npos);
    EXPECT_NE(sent[2].find("2 notification(s) dropped"), std::string::npos);
}

TEST(TelegramNotifier, StopAbandonsTheQueue) {
    boost::asio::io_context ioc;
    std::vector<std::string> sent;
    TelegramNotifier n(ioc, EnabledConfig(), [&sent](const std::string& t) { sent.push_back(t); });

    n.NotifyActivated("AAAUSDT", ActiveSet({"AAAUSDT"}));
    n.NotifyActivated("BBBUSDT", ActiveSet({"BBBUSDT"}));
    n.Stop();

    RunUntilIdle(ioc);

    // On shutdown the operator is watching the process exit, not the chat.
    EXPECT_EQ(sent.size(), 1u);
    EXPECT_EQ(n.QueueDepth(), 0u);
}

// The ONE test that uses the real transport, because it is the only way to
// check what the transport LOGS.
//
// The bot token lives in the URL path, and AsyncHttpsGet prints the target on
// every failure - so dropping the redacted log_target argument would write a
// live credential into the log on the first network blip. The host is in
// .invalid, which RFC 2606 reserves as never-resolvable, so this fails at DNS
// and never opens a socket.
TEST(TelegramNotifier, AFailedSendNeverLogsTheBotToken) {
    ScreenerConfig c = EnabledConfig();
    c.telegram_token = "123:SUPER-SECRET-TOKEN";
    c.telegram_host = "screener-telegram.invalid";

    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tlsv12_client);
    TelegramNotifier notifier(ioc, ssl_ctx, c);
    ASSERT_TRUE(notifier.Enabled());

    ::testing::internal::CaptureStdout();
    notifier.NotifyActivated("BTCUSDT", ActiveSet({"BTCUSDT"}));
    RunUntilIdle(ioc);
    const std::string logged = ::testing::internal::GetCapturedStdout();

    EXPECT_EQ(logged.find("SUPER-SECRET-TOKEN"), std::string::npos) << logged;
    EXPECT_NE(logged.find("<redacted>"), std::string::npos) << logged;
}

// ---------------------------------------------------------------------------
// Manual smoke test. DISABLED_, so a normal test run never sends a message or
// touches the network:
//
//   ./unit_tests --gtest_also_run_disabled_tests --gtest_filter='*SendsHello*'
//
// Reads the real .env, sends the literal text "hello" to the real chat, and
// prints whatever Telegram says. It asserts nothing about the SEND, because
// the outcome depends on a bot and a chat_id this process cannot verify - what
// it gives you is the response body, which is where Telegram explains a 4xx
// ("chat not found", "bot was blocked by the user", ...).
TEST(TelegramNotifierManual, DISABLED_SendsHelloToTheRealChat) {
    // The repo root, not the CWD: ctest runs this binary from the build tree.
    const std::string env_file = std::string(SCREENER_SOURCE_DIR) + "/.env";

    std::string flag = "--env-file=" + env_file;
    std::string telegram = "--telegram";
    std::string program = "unit_tests";
    char* argv[] = {program.data(), telegram.data(), flag.data()};

    const auto config = ScreenerConfig::FromArgs(3, argv);
    if (!config) {
        GTEST_SKIP() << "no usable " << env_file << " - see .env.example";
    }

    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tlsv12_client);
    load_root_certificates(ssl_ctx);
    ssl_ctx.set_verify_mode(boost::asio::ssl::verify_peer);

    TelegramNotifier notifier(ioc, ssl_ctx, *config);
    ASSERT_TRUE(notifier.Enabled());

    // A realistic startup message, so the manual run shows the actual layout
    // in the chat: bold, one symbol per line, with the not-ready gap named.
    TelegramNotifier::StartupReport report;
    report.tracked = 791;
    report.ready = 789;
    report.not_ready = {"AUSDT - no history (warm-up request failed)",
                        "NEWUSDT - 12 x 1h, 3 x 4h (EMA50 needs 50 x 4h)"};

    notifier.NotifyStarted(report, {"BTCUSDT", "ETHUSDT", "SOLUSDT"});

    // Long enough for DNS + TLS + the round trip, bounded so a hung socket
    // fails the test instead of the suite.
    ioc.run_for(std::chrono::seconds(20));

    std::fprintf(stderr, "sent a startup message to chat %s - check the chat, and the log above for a 4xx body\n",
                 config->telegram_chat_id.c_str());
}

}  // namespace screener
