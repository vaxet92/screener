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
            "usage: screener [--max-symbols=N] [--warmup-bars=N] [--topics-per-sub=N] [--verbose]\n"
            "\n"
            "  --max-symbols=N     cap the universe (0 = every linear USDT perpetual)\n"
            "  --warmup-bars=N     1h bars to fetch per symbol at startup (4..1000, default 603)\n"
            "  --topics-per-sub=N  topics per subscribe frame (default 100)\n"
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
