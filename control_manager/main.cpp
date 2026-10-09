#include <cstdio>

#include "config/config.h"
#include "control_manager/control_manager.h"
#include "logger/logger.h"

int main(int argc, char* argv[]) {
    // Line-buffer stdout explicitly.
    //
    // stdout is block-buffered whenever it is not a terminal, so piping the
    // screener into `tee` or a log file would withhold up to 4KB of output -
    // and on a long-running process that means the warm-up progress and the
    // ACTIVE/INACTIVE transitions appear in late bursts, or are lost entirely
    // if the process is killed. The transitions ARE the product, so they are
    // worth one flush per line.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    const auto config = screener::ScreenerConfig::FromArgs(argc, argv);
    if (!config) {
        std::fputs(
            "usage: screener [--max-symbols=N] [--warmup-bars=N] [--topics-per-sub=N] [--telegram]\n"
            "                [--env-file=PATH] [--verbose]\n"
            "\n"
            "  --max-symbols=N     cap the universe (0 = every linear USDT perpetual)\n"
            "  --warmup-bars=N     1h bars to fetch per symbol at startup (4..1000, default 603)\n"
            "  --topics-per-sub=N  topics per subscribe frame (default 100)\n"
            "  --telegram          send startup and ACTIVE/INACTIVE notifications to Telegram;\n"
            "                      requires TELEGRAM_BOT_TOKEN and TELEGRAM_CHAT_ID in .env or\n"
            "                      the environment (NOT flags - argv is visible via ps)\n"
            "  --env-file=PATH     secrets file (default .env; see .env.example). A missing\n"
            "                      default is fine, a missing PATH you named is not\n"
            "  --verbose           debug logging\n",
            stderr);
        return 2;
    }

    if (config->verbose) {
        Logger::SetMinLevel(LogLevel::kDebug);
    }

    screener::ControlManager manager(*config);
    return manager.Run();
}
