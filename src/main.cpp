//
// Created by Kotarou on 2022/4/5.
//
#include <csignal>
#include <cstdlib>

#include <spdlog/spdlog.h>

#include "cli/cli.h"
#include "config/config.h"
#include "core/manager.h"
#include "logging_pattern.h"
#include "exceptions/base_exception.h"
#include "exceptions/config_verification_exception.h"

namespace {
    void block_signals() {
        sigset_t sigset;
        sigemptyset(&sigset);
        sigaddset(&sigset, SIGINT);
        sigaddset(&sigset, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &sigset, nullptr);
    }
}

int main(int argc, char *argv[]) {
    const auto outcome = cli::parse_and_dispatch(argc, argv);

    // Global logging initialisation.
    spdlog::set_pattern(YADDNSC_LOGGING_PATTERN);
    spdlog::set_level(outcome.verbose ? spdlog::level::debug : spdlog::level::info);

    if (!outcome.should_run) {
        return outcome.exit_code;
    }

    // ── RUN flow ──────────────────────────────────────────────────────────
    if (outcome.verbose) {
        SPDLOG_DEBUG("Verbose mode enabled");
    }

    block_signals();
    try {
        auto config = Config::load_config(outcome.config_path);
        const Manager manager(std::move(config));
        manager.load_drivers();
        manager.validate_config();

        manager.install_signal_handler();
        manager.run();
        return EXIT_SUCCESS;
    } catch (const ConfigVerificationException &e) {
        SPDLOG_CRITICAL(e.what());
    } catch (const YaddnscException &) {
        SPDLOG_CRITICAL("Fatal error: unrecoverable exception.");
    } catch (const std::exception &e) {
        SPDLOG_CRITICAL("Unhandled exception. Error: {}", e.what());
    }

    return EXIT_FAILURE;
}
