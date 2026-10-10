// Dotenv parsing, and the precedence rule around it.
//
// ApplyDotEnv is tested on CONTENT, with no filesystem: it is a parser, and a
// parser's failure modes are its lines, not its inode. The precedence test
// goes through FromArgs with a real temporary file, because "the shell wins
// over the file" is a property of the ORDER of two reads and cannot be
// observed in the parser alone.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "config/config.h"

namespace screener {
namespace {

ScreenerConfig Parse(const std::string& content, std::string& error) {
    ScreenerConfig config;
    error = ApplyDotEnv(content, config);
    return config;
}

// argv is char*, and FromArgs takes it as such.
std::optional<ScreenerConfig> FromArgs(std::vector<std::string> args) {
    std::vector<char*> argv;
    std::string program = "screener";
    argv.push_back(program.data());
    for (std::string& a : args) {
        argv.push_back(a.data());
    }
    return ScreenerConfig::FromArgs(static_cast<int>(argv.size()), argv.data());
}

std::string WriteTempEnv(const std::string& content) {
    const std::string path = std::string(testing::TempDir()) + "screener_test.env";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    return path;
}

ScreenerConfig ParseJson(const std::string& content, std::string& error) {
    ScreenerConfig config;
    error = ApplyConfigJson(content, config);
    return config;
}

std::string WriteTempConfigJson(const std::string& content) {
    const std::string path = std::string(testing::TempDir()) + "screener_test_config.json";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    return path;
}

}  // namespace

TEST(DotEnv, ReadsBothSecrets) {
    std::string error;
    const ScreenerConfig c = Parse("TELEGRAM_BOT_TOKEN=123:AAA\nTELEGRAM_CHAT_ID=-1001\n", error);

    EXPECT_TRUE(error.empty()) << error;
    EXPECT_EQ(c.telegram_token, "123:AAA");
    EXPECT_EQ(c.telegram_chat_id, "-1001");
}

TEST(DotEnv, SkipsCommentsBlankLinesAndWhitespace) {
    std::string error;
    const ScreenerConfig c = Parse(
        "# a comment\n"
        "\n"
        "   \n"
        "  TELEGRAM_BOT_TOKEN = 123:AAA  \n"
        "\t# indented comment\n",
        error);

    EXPECT_TRUE(error.empty()) << error;
    EXPECT_EQ(c.telegram_token, "123:AAA");
}

TEST(DotEnv, AcceptsQuotesAndAnExportPrefix) {
    std::string error;
    // Quoting is how a chat_id starting with '-' gets written, and `export
    // KEY=VALUE` is what a line copied out of a shell looks like.
    const ScreenerConfig c = Parse("export TELEGRAM_CHAT_ID=\"-1001234567890\"\nTELEGRAM_BOT_TOKEN='123:AAA'\n", error);

    EXPECT_TRUE(error.empty()) << error;
    EXPECT_EQ(c.telegram_chat_id, "-1001234567890");
    EXPECT_EQ(c.telegram_token, "123:AAA");
}

TEST(DotEnv, HandlesCrlfAndAMissingFinalNewline) {
    std::string error;
    const ScreenerConfig c = Parse("TELEGRAM_BOT_TOKEN=123:AAA\r\nTELEGRAM_CHAT_ID=-1001", error);

    EXPECT_TRUE(error.empty()) << error;
    EXPECT_EQ(c.telegram_token, "123:AAA");
    EXPECT_EQ(c.telegram_chat_id, "-1001");
}

TEST(DotEnv, StripsAnInlineCommentAfterAnUnquotedValue) {
    std::string error;
    const ScreenerConfig c = Parse("TELEGRAM_BOT_TOKEN=123:AAA  # from @BotFather\n", error);

    EXPECT_TRUE(error.empty()) << error;
    EXPECT_EQ(c.telegram_token, "123:AAA");
}

TEST(DotEnv, StripsAnInlineCommentAfterAQuotedValue) {
    std::string error;
    // The regression that produced a 400: with a comment after the closing
    // quote, "strip matching outer quotes" never fires, and the token kept
    // its quotes AND the comment - spaces and all - straight into the URL.
    const ScreenerConfig c = Parse(
        "TELEGRAM_BOT_TOKEN = \"123:AAA\"  # from @BotFather\n"
        "TELEGRAM_CHAT_ID = '-1001'  # from @userinfobot\n",
        error);

    EXPECT_TRUE(error.empty()) << error;
    EXPECT_EQ(c.telegram_token, "123:AAA");
    EXPECT_EQ(c.telegram_chat_id, "-1001");
}

TEST(DotEnv, KeepsAHashInsideQuotes) {
    std::string error;
    const ScreenerConfig c = Parse("TELEGRAM_BOT_TOKEN=\"123:AA#BB\"\n", error);

    EXPECT_TRUE(error.empty()) << error;
    EXPECT_EQ(c.telegram_token, "123:AA#BB");
}

TEST(DotEnv, RejectsAnUnterminatedQuote) {
    std::string error;
    Parse("TELEGRAM_BOT_TOKEN=\"123:AAA\n", error);
    EXPECT_FALSE(error.empty());
}

TEST(DotEnv, RejectsJunkAfterAQuotedValue) {
    std::string error;
    Parse("TELEGRAM_BOT_TOKEN=\"123:AAA\" junk\n", error);
    EXPECT_FALSE(error.empty());
}

TEST(DotEnvFile, RejectsASecretThatCannotGoInAUrl) {
    // Belt and braces for the same failure: even if a secret reaches the
    // config with a space or a quote in it, the run stops with a message that
    // names the cause instead of a 400 an hour later.
    const std::string path = WriteTempEnv("TELEGRAM_BOT_TOKEN='123 AAA'\nTELEGRAM_CHAT_ID=-1001\n");

    ::unsetenv("TELEGRAM_BOT_TOKEN");
    ::unsetenv("TELEGRAM_CHAT_ID");

    const auto c = FromArgs({"--telegram", "--env-file=" + path});
    std::remove(path.c_str());

    EXPECT_FALSE(c.has_value());
}

TEST(DotEnv, IgnoresKeysItDoesNotOwn) {
    std::string error;
    // A .env is shared with whatever else runs in this directory. Failing on
    // another tool's variable would be this process overreaching.
    const ScreenerConfig c = Parse("DATABASE_URL=postgres://x\nTELEGRAM_BOT_TOKEN=123:AAA\nPATH_EXTRA=/opt\n", error);

    EXPECT_TRUE(error.empty()) << error;
    EXPECT_EQ(c.telegram_token, "123:AAA");
}

TEST(DotEnv, RejectsALineWithNoEquals) {
    std::string error;
    Parse("TELEGRAM_BOT_TOKEN=123:AAA\nTELEGRAM_CHAT_ID -1001\n", error);

    // A typo in a secrets file stops the run, exactly like an unknown flag.
    ASSERT_FALSE(error.empty());
    EXPECT_NE(error.find("line 2"), std::string::npos) << error;
}

TEST(DotEnv, RejectsAnEmptyValueForAKeyItOwns) {
    std::string error;
    Parse("TELEGRAM_BOT_TOKEN=\n", error);

    // Not a default: the operator meant to set it. Silently empty would show
    // up as "--telegram needs TELEGRAM_BOT_TOKEN" with the variable sitting
    // right there in the file.
    ASSERT_FALSE(error.empty());
    EXPECT_NE(error.find("TELEGRAM_BOT_TOKEN"), std::string::npos) << error;
}

TEST(DotEnv, RejectsAnEmptyKey) {
    std::string error;
    Parse("=value\n", error);
    EXPECT_FALSE(error.empty());
}

TEST(DotEnvFile, TheShellWinsOverTheFile) {
    const std::string path = WriteTempEnv("TELEGRAM_BOT_TOKEN=from_file\nTELEGRAM_CHAT_ID=-1001\n");

    ::setenv("TELEGRAM_BOT_TOKEN", "from_shell", 1);
    ::unsetenv("TELEGRAM_CHAT_ID");

    const auto c = FromArgs({"--telegram", "--env-file=" + path});

    ::unsetenv("TELEGRAM_BOT_TOKEN");
    std::remove(path.c_str());

    ASSERT_TRUE(c.has_value());
    // The file supplies defaults; an explicit export redirects one run.
    EXPECT_EQ(c->telegram_token, "from_shell");
    EXPECT_EQ(c->telegram_chat_id, "-1001");
}

TEST(DotEnvFile, AnExplicitPathThatCannotBeReadIsFatal) {
    ::unsetenv("TELEGRAM_BOT_TOKEN");
    ::unsetenv("TELEGRAM_CHAT_ID");

    // Asymmetric on purpose: a missing DEFAULT .env means "use the
    // environment", but a path the operator typed must exist.
    EXPECT_FALSE(FromArgs({"--env-file=/nonexistent/screener/.env"}).has_value());
}

TEST(DotEnvFile, TelegramWithoutSecretsIsRefused) {
    const std::string path = WriteTempEnv("# nothing here\n");

    ::unsetenv("TELEGRAM_BOT_TOKEN");
    ::unsetenv("TELEGRAM_CHAT_ID");

    const auto c = FromArgs({"--telegram", "--env-file=" + path});
    std::remove(path.c_str());

    // --telegram is an explicit request; honouring it as a no-op is the
    // failure nobody notices until the first ACTIVE never arrives.
    EXPECT_FALSE(c.has_value());
}

TEST(DotEnvFile, SecretsWithoutTheFlagLeaveNotificationsOff) {
    const std::string path = WriteTempEnv("TELEGRAM_BOT_TOKEN=123:AAA\nTELEGRAM_CHAT_ID=-1001\n");

    const auto c = FromArgs({"--env-file=" + path});
    std::remove(path.c_str());

    ASSERT_TRUE(c.has_value());
    EXPECT_FALSE(c->telegram_enabled);
}

TEST(ConfigJson, ReadsAllFiveThresholdFields) {
    std::string error;
    const ScreenerConfig c = ParseJson(
        R"({"ema_period": 20, "surge_numerator": 150, "surge_denominator": 100,
            "min_natr_bp": 80, "min_turnover_usdt": "250000.50"})",
        error);

    EXPECT_TRUE(error.empty()) << error;
    EXPECT_EQ(c.ema_period, 20u);
    EXPECT_EQ(c.surge_numerator, 150);
    EXPECT_EQ(c.surge_denominator, 100);
    EXPECT_EQ(c.min_natr_bp, 80);
    EXPECT_EQ(c.min_turnover, 250000500000);  // 250000.50 * kVolumeScale (1e6)
}

TEST(ConfigJson, LeavesUnmentionedFieldsAtTheCompiledDefault) {
    std::string error;
    const ScreenerConfig c = ParseJson(R"({"ema_period": 20})", error);

    EXPECT_TRUE(error.empty()) << error;
    EXPECT_EQ(c.ema_period, 20u);
    EXPECT_EQ(c.surge_numerator, 130);   // unmentioned: compiled default
    EXPECT_EQ(c.surge_denominator, 100);  // unmentioned: compiled default
}

TEST(ConfigJson, IgnoresASlashSlashPrefixedCommentKey) {
    // Plain JSON has no comment syntax; "//"-prefixed keys are the one
    // escape from "unrecognised key is fatal", so example_config.json can
    // document a field without becoming unusable as a direct copy to
    // config.json.
    std::string error;
    const ScreenerConfig c = ParseJson(R"({"// ema_period": "HTF EMA period", "ema_period": 20})", error);

    EXPECT_TRUE(error.empty()) << error;
    EXPECT_EQ(c.ema_period, 20u);
}

TEST(ConfigJson, RejectsAnUnrecognisedKey) {
    std::string error;
    // Unlike .env, nothing else shares this file - a typo should be heard.
    ParseJson(R"({"ema_perido": 20})", error);

    ASSERT_FALSE(error.empty());
    EXPECT_NE(error.find("ema_perido"), std::string::npos) << error;
}

TEST(ConfigJson, RejectsMalformedJson) {
    std::string error;
    ParseJson("{not json", error);
    EXPECT_FALSE(error.empty());
}

TEST(ConfigJson, RejectsATopLevelArray) {
    std::string error;
    ParseJson("[1, 2, 3]", error);
    EXPECT_FALSE(error.empty());
}

TEST(ConfigJson, RejectsEmaPeriodOfOneOrLess) {
    std::string error;
    ParseJson(R"({"ema_period": 1})", error);
    EXPECT_FALSE(error.empty());
}

TEST(ConfigJson, RejectsATurnoverFloorGivenAsANumberNotAString) {
    // min_turnover_usdt must be a decimal STRING - a bare JSON number with a
    // fractional part would have to go through a double to reach simdjson's
    // get_uint64/get_double, reintroducing the rounding error the scaled
    // integer exists to avoid.
    std::string error;
    ParseJson(R"({"min_turnover_usdt": 250000.50})", error);
    EXPECT_FALSE(error.empty());
}

TEST(ConfigJsonFile, AnExplicitPathThatCannotBeReadIsFatal) {
    ::unsetenv("TELEGRAM_BOT_TOKEN");
    ::unsetenv("TELEGRAM_CHAT_ID");

    EXPECT_FALSE(FromArgs({"--config=/nonexistent/screener/config.json"}).has_value());
}

TEST(ConfigJsonFile, AnExplicitValidFileIsApplied) {
    const std::string path = WriteTempConfigJson(R"({"ema_period": 30})");

    ::unsetenv("TELEGRAM_BOT_TOKEN");
    ::unsetenv("TELEGRAM_CHAT_ID");

    const auto c = FromArgs({"--config=" + path});
    std::remove(path.c_str());

    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->ema_period, 30u);
}

TEST(ConfigJsonFile, AMalformedExplicitFileIsFatal) {
    const std::string path = WriteTempConfigJson("{not json");

    ::unsetenv("TELEGRAM_BOT_TOKEN");
    ::unsetenv("TELEGRAM_CHAT_ID");

    const auto c = FromArgs({"--config=" + path});
    std::remove(path.c_str());

    EXPECT_FALSE(c.has_value());
}

TEST(Validate, RejectsASurgeRatioOfOneOrLess) {
    ScreenerConfig config;
    config.surge_numerator = 100;
    config.surge_denominator = 100;
    EXPECT_FALSE(config.Validate().empty());
}

}  // namespace screener
