//
// Created by Kotarou on 2022/4/7.
//

#include "manager.h"

#include <algorithm>
#include <fmt/format.h>

#include "worker.h"
#include "context.h"
#include "ip_util.h"
#include "network_util.h"
#include "spdlog/spdlog.h"

#include "exception/config_verification_exception.h"

template<typename T>
void dedupe(std::vector<T> &vec) {
    std::sort(vec.begin(), vec.end());
    vec.erase(std::unique(vec.begin(), vec.end()), vec.end());
}

void Manager::validate_config() {
    auto &context = Context::getInstance();

    auto drivers = context.driver_manager->get_loaded_drivers();
    auto interfaces = NetworkUtil::get_interfaces();

    for (const auto &domain: _config.domains) {
        // check drivers
        if (std::find(drivers.begin(), drivers.end(), domain.driver) == drivers.end()) {
            throw ConfigVerificationException(fmt::format("Driver {} not found", domain.driver));
        }

        // check update interval
        if (domain.update_interval < MIN_UPDATE_INTERVAL) {
            throw ConfigVerificationException(
                    fmt::format("Update interval too low for domain {} ({}), minimal interval: {}", domain.name,
                                domain.update_interval, MIN_UPDATE_INTERVAL));
        }

        // check force update interval
        if (domain.force_update != 0 && domain.force_update < domain.update_interval) {
            throw ConfigVerificationException(
                    fmt::format("Force update interval for domain {} must not be smaller than the update interval ({})",
                                domain.name, domain.update_interval));
        }

        // check interfaces
        for (auto &subdomain: domain.subdomains) {
            if (!subdomain.interface.empty()) {
                if (std::find(interfaces.begin(), interfaces.end(), subdomain.interface) == interfaces.end()) {
                    throw ConfigVerificationException(fmt::format("Interface {} not found", subdomain.interface));
                }
            }
        }
    }

    // check resolver
    if (_config.resolver.use_customise_server) {
        auto &address = _config.resolver.ip_address;
        if (!IPUtil::is_ipv4_address(address) && !IPUtil::is_ipv6_address(address)) {
            throw ConfigVerificationException(fmt::format("Invalid resolver address {}", address));
        }
    }
}

void Manager::load_drivers() const {
    auto &context = Context::getInstance();
    auto &driver_manager = context.driver_manager;

    // remove duplicated lines
    std::vector<std::string> load(_config.driver.load);
    dedupe(load);

    // load drivers
    for (auto &driver: load) {
        driver_manager->load_driver(driver);
    }
}

void Manager::create_worker() {
    for (const auto &domain: _config.domains) {
        _workers.emplace_back(domain);
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
