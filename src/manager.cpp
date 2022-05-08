//
// Created by Kotarou on 2022/4/7.
//

#include "manager.h"

#include <algorithm>
#include <filesystem>
#include <unordered_set>
#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <config_cmake.h>

#include "dns.h"
#include "worker.h"
#include "context.h"
#include "ip_util.h"
#include "network_util.h"
#include "driver_manager.h"

#include "exception/config_verification_exception.h"

class Manager::Impl {
public:
    explicit Impl(Config::config config) : config_(std::move(config)) {};

    ~Impl() = default;

    template<typename T>
    void dedupe(std::vector<T> &vec) const {
        std::unordered_set<T> seen;

        auto end = std::remove_if(vec.begin(), vec.end(), [&seen](const T &value) {
            if (seen.find(value) != seen.end())
                return true;

            seen.insert(value);
            return false;
        });

        vec.erase(end, vec.end());
    }

    unsigned int estimated_threads();

public:
    static constexpr int MIN_UPDATE_INTERVAL = 60;

    Config::config config_;

    std::vector<Worker> workers_;
};

unsigned int Manager::Impl::estimated_threads() {
    unsigned int total_subdomains = 0;
    auto thread_count = std::thread::hardware_concurrency();

    for (auto &domain: config_.domains) {
        total_subdomains += static_cast<unsigned int>(domain.subdomains.size());
    }

    if (total_subdomains < 2 || thread_count < 2) {
        return 2;
    } else if (total_subdomains < thread_count) {
        return total_subdomains;
    }

    return thread_count;
}

void Manager::validate_config() {
    auto &context = Context::getInstance();

    auto drivers = context.driver_manager_->get_loaded_drivers();
    auto interfaces = NetworkUtil::get_interfaces();

    for (const auto &domain: impl_->config_.domains) {
        // check drivers
        if (std::find(drivers.begin(), drivers.end(), domain.driver) == drivers.end()) {
            throw ConfigVerificationException(fmt::format("Driver {} not found", domain.driver));
        }

        // check update interval
        if (domain.update_interval < impl_->MIN_UPDATE_INTERVAL) {
            throw ConfigVerificationException(
                    fmt::format("Update interval too low for domain {} ({}), minimal interval: {}", domain.name,
                                domain.update_interval, impl_->MIN_UPDATE_INTERVAL));
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

#ifdef HAVE_RES_NQUERY
    // check resolver
    if (impl_->config_.resolver.use_custom_server) {
        auto &address = impl_->config_.resolver.ip_address;
#ifdef HAVE_IPV6_RESOLVE_SUPPORT
        if (!IPUtil::is_ipv4_address(address) && !IPUtil::is_ipv6_address(address)) {
            throw ConfigVerificationException(fmt::format("Invalid resolver address {}", address));
        }
#else
        if (!IPUtil::is_ipv4_address(address)) {
            throw ConfigVerificationException(
                    fmt::format(R"(Invalid resolver address "{}". Only IPv4 is supported on your platform.)",
                                address));
        }
#endif
    }
#endif
}

void Manager::load_drivers() const {
    auto &context = Context::getInstance();
    auto &driver_manager = context.driver_manager_;

    // remove duplicated lines
    auto load{impl_->config_.driver.load};
    impl_->dedupe(load);

    // load drivers
    auto base_dir = std::filesystem::path(impl_->config_.driver.driver_dir);
    for (auto &driver: load) {
        auto driver_full_path = base_dir.empty() ? std::filesystem::path(driver) : base_dir / driver;
        driver_manager->load_driver(driver_full_path.string());
    }
}

void Manager::create_worker() {
    for (const auto &domain: impl_->config_.domains) {
        impl_->workers_.emplace_back(domain, impl_->config_.resolver);
    }
}

void Manager::run() {
    // print all interfaces name
    auto interfaces = NetworkUtil::get_interfaces();
    SPDLOG_INFO("All available interfaces: {}", fmt::join(interfaces, ", "));

    if (impl_->config_.resolver.use_custom_server) {
#ifdef HAVE_RES_NQUERY
        const auto &ip_addr = impl_->config_.resolver.ip_address;
        if (IPUtil::is_ipv4_address(ip_addr)) {
            SPDLOG_INFO(R"(Use custom resolver "{}:{}")", ip_addr, impl_->config_.resolver.port);
        } else if (ip_addr.front() != '[' && ip_addr.back() != ']') {
            SPDLOG_INFO(R"(Use custom resolver "[{}]:{}")", ip_addr, impl_->config_.resolver.port);
        }
#else
        SPDLOG_WARN(
                "Custom resolver defined, but res_nquery not support on your platform, this option will be ignored");
#endif
    }

    // set worker concurrency level
    Worker::set_concurrency(impl_->estimated_threads());

    // create worker threads
    std::vector<std::thread> worker_threads;
    std::transform(impl_->workers_.begin(), impl_->workers_.end(), std::back_inserter(worker_threads),
                   [](auto &worker) {
                       return std::thread(&Worker::run, std::addressof(worker));
                   }
    );

    // all clear, join workers in order to block main.
    for (auto &worker: worker_threads) {
        worker.join();
    }
}

Manager::Manager(Config::config config) : impl_(new Impl(std::move(config))) {

}

void Manager::ImplDeleter::operator()(Manager::Impl *ptr) {
    delete ptr;
}
