//
// Created by Kotarou on 2022/4/5.
//
#include <iostream>
#include "stop_token_compat.h"

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>

#include "config.h"
#include "manager.h"
#include "app_context.h"
#include "version.h"
#include "signal_handler.h"
#include "logging_pattern.h"
#include "driver_manager.h"
#include "network_manager.h"

#include "exception/base_exception.h"
#include "exception/config_verification_exception.h"

namespace {
    std::stop_source g_stop_source;
}

void gracefully_quit();
std::shared_ptr<AppContext> init_context();

int main(int argc, char *argv[]) {
    // blocked signals, will be process by signal handling thread
    sigset_t sigset = SignalHandler::block_signal({SIGINT, SIGTERM});
    SignalHandler::register_handler(SIGINT, &gracefully_quit);
    SignalHandler::register_handler(SIGTERM, &gracefully_quit);

    // signal handling thread
    auto sig_thread = std::thread(SignalHandler::handler_thread, &sigset);
    sig_thread.detach();

    // CLI parsing
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
            exit(0);
        }

        if (result.count("version")) {
            std::cout << yaddnsc::get_full_version() << std::endl;
            exit(0);
        }

        // logging
        spdlog::set_pattern(YADDNSC_LOGGING_PATTERN);
        if (result["verbose"].as<bool>()) {
            spdlog::set_level(spdlog::level::debug);
            SPDLOG_DEBUG("Verbose mode enabled");
        } else {
            spdlog::set_level(spdlog::level::info);
        }

        auto app_ctx = init_context();

        auto config = Config::load_config(config_path);

        Manager manager(app_ctx, config);

        // load all drivers
        manager.load_drivers();

        // check the config file
        manager.validate_config();

        // create workers
        manager.create_worker();

        // main event loop
        manager.run(g_stop_source.get_token());

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

void gracefully_quit() {
    SPDLOG_INFO("Received exit signal, quitting...");
    if (g_stop_source.stop_possible()) {
        if (!g_stop_source.request_stop()) {
            SPDLOG_ERROR("Request stop failed.");
        }
    }
}

std::shared_ptr<AppContext> init_context() {
    auto app_ctx = std::make_shared<AppContext>();

    app_ctx->driver_manager_ = std::make_unique<DriverManager>();
    app_ctx->network_manager_ = std::make_unique<NetworkManager>();

    return app_ctx;
}