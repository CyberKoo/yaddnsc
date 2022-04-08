//
// Created by Kotarou on 2022/4/5.
//

#include <csignal>

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>

#include "config.h"
#include "context.h"
#include "manager.h"

void sigint_handler([[maybe_unused]] int signal) {
    SPDLOG_INFO("Received exit signal, quiting...");
    Context::getInstance().terminate = true;
    Context::getInstance().cv.notify_all();
}

int main(int argc, char *argv[]) {
    auto &context = Context::getInstance();
    signal(SIGINT, sigint_handler);

    cxxopts::Options options("yaddnsc", "Yet another DDNS client");
    options.add_options()
            ("v,verbose", "enable verbose mode")
            ("c,config", "Config file path",
             cxxopts::value(context.config_path)->default_value("./config.json"))
            ("V,version", "print version")
            ("h,help", "Print usage");

    try {
        auto result = options.parse(argc, argv);
        if (result.count("help")) {
            std::cout << options.help() << std::endl;
            exit(0);
        }

        // logging
        spdlog::set_pattern("[%D %T.%e] [%^%8l%$] [%15s:%#] %v");
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
    } catch (cxxopts::OptionException &e) {
        SPDLOG_CRITICAL(e.what());
    } catch (std::exception &e) {
        SPDLOG_CRITICAL("Unhandled exception, error: {}", e.what());
    }
}