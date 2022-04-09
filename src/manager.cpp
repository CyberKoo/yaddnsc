//
// Created by Kotarou on 2022/4/7.
//

#include "manager.h"

#include <mutex>
#include <algorithm>
#include <fmt/format.h>

#include "worker.h"
#include "context.h"
#include "network_util.h"
#include "spdlog/spdlog.h"

void Manager::validate_config() {
    auto &context = Context::getInstance();

    // check drivers
    auto drivers = context.driver_manager->get_loaded_drivers();
    for (auto &domain: _config.domains) {
        if (std::find(drivers.begin(), drivers.end(), domain.driver) == drivers.end()) {
            throw std::runtime_error(fmt::format("Driver {} not found", domain.driver));
        }
    }

    // check interfaces
    auto interfaces = NetworkUtil::get_interfaces();
    for (auto &domain: _config.domains) {
        for (auto &subdomain: domain.subdomains) {
            if (!subdomain.interface.empty()) {
                if (std::find(interfaces.begin(), interfaces.end(), subdomain.interface) == interfaces.end()) {
                    throw std::runtime_error(fmt::format("Interface {} not found", subdomain.interface));
                }
            }
        }
    }
}

void Manager::load_drivers() const {
    auto &context = Context::getInstance();
    auto &driver_manager = context.driver_manager;

    // load drivers
    for (auto &driver: _config.driver.load) {
        driver_manager->load_driver(driver);
    }
}

void Manager::create_worker() {
    for (const auto &domain: _config.domains) {
        _workers.emplace_back(Worker{{domain}});
    }
}

void Manager::run() {
    auto &context = Context::getInstance();
    // move resolver_config to context
    context.resolver_config = std::move(_config.resolver);
    _config.resolver = {};

    // print all interfaces name
    auto interfaces = NetworkUtil::get_interfaces();
    SPDLOG_INFO("All available interface: {}", fmt::join(interfaces, ", "));

    // create worker threads
    std::vector<std::thread> worker_threads;
    std::transform(_workers.begin(), _workers.end(), std::back_inserter(worker_threads),
                   [](auto &worker) {
                       return std::thread(&Worker::run, std::addressof(worker));
                   }
    );

    // all clear, join workers in order to block main.
    for (auto &worker: worker_threads) {
        worker.join();
    }
}

