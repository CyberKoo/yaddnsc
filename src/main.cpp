//
// Created by Kotarou on 2022/4/5.
//

#include <csignal>
#include <cxxopts.hpp>
#include <spdlog/spdlog.h>

#include "config.h"
#include "context.h"
#include "manager.h"
#include "version.h"
#include "logging_pattern.h"

#include "exception/base_exception.h"
#include "exception/config_verification_exception.h"

void sigint_handler([[maybe_unused]] int signal) {
    auto &context = Context::getInstance();
    SPDLOG_INFO("Received exit signal, quiting...");
    context.terminate = true;
    context.cv.notify_all();
}

int main(int argc, char *argv[]) {
    auto &context = Context::getInstance();
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    cxxopts::Options options("yaddnsc", "Yet another DDNS client");
    options.add_options()
            ("v,verbose", "Enable verbose mode")
            ("c,config", "Config file path", cxxopts::value(context.config_path)->default_value("./config.json"))
            ("V,version", "Print version")
            ("h,help", "Print usage");

    try {
        auto result = options.parse(argc, argv);
        if (result.count("help")) {
            std::cout << options.help() << std::endl;
            exit(0);
        }

        if (result.count("version")) {
            std::cout << "YADDNSC/" << get_version() << std::endl;
            exit(0);
        }

        // logging
        spdlog::set_pattern(YADDNSC_LOGGING_PATTERN);
        if (result["verbose"].as<bool>()) {
            spdlog::set_level(spdlog::level::debug);
            SPDLOG_DEBUG("verbose mode enabled");
        } else {
            spdlog::set_level(spdlog::level::info);
        }

        auto config = Config::load_config(context.config_path);

        Manager manager(config);

        // load all drivers
        manager.load_drivers();

        // check the config file
        manager.validate_config();

        // create workers
        manager.create_worker();

        // main event loop
        manager.run();

        return 0;
    } catch (ConfigVerificationException &e) {
        SPDLOG_CRITICAL(e.what());
    } catch (YaddnscException &e) {
        SPDLOG_CRITICAL("Program crashed due to an unrecoverable error.");
    } catch (cxxopts::OptionException &e) {
        SPDLOG_CRITICAL(e.what());
    } catch (std::exception &e) {
        SPDLOG_CRITICAL("Unhandled exception, error: {}", e.what());
    }

    return -1;
}