//
// Created by Kotarou on 2022/4/5.
//
#include <iostream>
#include <csignal>

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>

#include "config.h"
#include "manager.h"
#include "version.h"
#include "logging_pattern.h"

#include "exception/base_exception.h"
#include "exception/config_verification_exception.h"

int main(int argc, char *argv[]) {
    // Block SIGINT and SIGTERM in all threads so they can be handled by
    // a dedicated sigwait() thread inside Manager::run() instead of the
    // default handler (which would kill the process immediately).
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGINT);
    sigaddset(&sigset, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigset, nullptr);

    // CLI parsing.
    std::string config_path = "./config.json";
    cxxopts::Options options("yaddnsc", "Yet another DDNS client");
    options.add_options()
            ("v,verbose", "Enable verbose mode")
            ("c,config", "Config file path", cxxopts::value(config_path)->default_value("./config.json"))
            ("V,version", "Print version")
            ("h,help", "Print usage");

    try {
        auto result = options.parse(argc, argv);
        if (result.count("help")) {
            std::cout << options.help() << std::endl;
            return 0;
        }

        if (result.count("version")) {
            std::cout << yaddnsc::get_full_version() << std::endl;
            return 0;
        }

        // Logging.
        spdlog::set_pattern(YADDNSC_LOGGING_PATTERN);
        if (result["verbose"].as<bool>()) {
            spdlog::set_level(spdlog::level::debug);
            SPDLOG_DEBUG("Verbose mode enabled");
        } else {
            spdlog::set_level(spdlog::level::info);
        }

        auto config = Config::load_config(config_path);

        Manager manager(std::move(config));

        // Load all drivers.
        manager.load_drivers();

        // Validate the config file.
        manager.validate_config();

        // Install a signal-handling thread that catches SIGINT/SIGTERM
        // and requests a graceful shutdown.
        manager.install_signal_handler();

        // Main event loop (single scheduler thread + shared thread pool).
        manager.run();

        return 0;
    } catch (ConfigVerificationException &e) {
        SPDLOG_CRITICAL(e.what());
    } catch (YaddnscException &) {
        SPDLOG_CRITICAL("Program crashed due to an unrecoverable error.");
    } catch (cxxopts::exceptions::exception &e) {
        SPDLOG_CRITICAL(e.what());
    } catch (std::exception &e) {
        SPDLOG_CRITICAL("Unhandled exception. Error: {}", e.what());
    }

    return -1;
}
