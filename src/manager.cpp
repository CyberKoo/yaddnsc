//
// Created by Kotarou on 2022/4/7.
//

#include "manager.h"

#include <algorithm>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "worker.h"
#include "context.h"
#include "ip_util.h"
#include "network_util.h"
#include "driver_manager.h"

#include "exception/config_verification_exception.h"

class Manager::Impl {
public:
    explicit Impl(Config::config_t config) : _config(std::move(config)) {};

    ~Impl() = default;

    void validate_config();

    void load_drivers() const;

    void create_worker();

    void run();

private:
    static constexpr int MIN_UPDATE_INTERVAL = 60;

    Config::config_t _config;

    std::vector<Worker> _workers;

private:
    template<typename T>
    void dedupe(std::vector<T> &vec) const {
        std::sort(vec.begin(), vec.end());
        vec.erase(std::unique(vec.begin(), vec.end()), vec.end());
    }
};

void Manager::Impl::validate_config() {
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

void Manager::Impl::load_drivers() const {
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

void Manager::Impl::create_worker() {
    for (const auto &domain: _config.domains) {
        _workers.emplace_back(domain, _config.resolver);
    }
}

void Manager::Impl::run() {
    // print all interfaces name
    auto interfaces = NetworkUtil::get_interfaces();
    SPDLOG_INFO("All available interface: {}", fmt::join(interfaces, ", "));

    if (_config.resolver.use_customise_server) {
#ifdef USE_RES_NQUERY
        SPDLOG_INFO("Use customized resolver \"{}:{}\"", _config.resolver.ip_address, _config.resolver.port);
#elif
        SPDLOG_WARN("Custom resolver defined, but res_nquery not support on your system, this option will be ignored");
#endif
    }

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

Manager::Manager(Config::config_t config) : _impl(new Impl(std::move(config))) {

}

void Manager::validate_config() {
    return _impl->validate_config();
}

void Manager::load_drivers() const {
    return _impl->load_drivers();
}

void Manager::create_worker() {
    return _impl->create_worker();
}

void Manager::run() {
    return _impl->run();
}

void Manager::ImplDeleter::operator()(Manager::Impl *ptr) {
    delete ptr;
}
