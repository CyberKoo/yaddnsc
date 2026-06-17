//
// Created by Kotarou on 2022/4/7.
//

#include "manager.h"

#include <thread>
#include <algorithm>
#include <filesystem>
#include <unordered_set>
#include <stop_token>

#include "fmt.h"
#include <spdlog/spdlog.h>
#include <config_cmake.h>

#include "dns.h"
#include "worker.h"
#include "context.h"
#include "ip_util.h"
#include "network_manager.h"
#include "driver_manager.h"

#include "exception/config_verification_exception.h"

class Manager::Impl {
public:
    explicit Impl(std::shared_ptr<AppContext> app_ctx, Config::config config)
        : app_ctx_(std::move(app_ctx)), config_(std::move(config)) {
    }

    ~Impl() = default;

    template<typename T>
    void dedupe(std::vector<T> &vec) const {
        std::unordered_set<T> seen;
        std::erase_if(vec, [&seen](const T &value) {
            return !seen.insert(value).second;
        });
    }

    unsigned int estimated_threads() const;

public:
    static constexpr int MIN_UPDATE_INTERVAL = 60;

    std::shared_ptr<AppContext> app_ctx_;

    Config::config config_;

    std::vector<Worker> workers_;
};

unsigned int Manager::Impl::estimated_threads() const {
    unsigned int total_subdomains = 0;
    auto thread_count = std::thread::hardware_concurrency();

    for (auto &domain: config_.domains) {
        total_subdomains += static_cast<unsigned int>(domain.subdomains.size());
    }

    if (total_subdomains < 2 || thread_count < 2) {
        return 2;
    }

    if (total_subdomains < thread_count) {
        return total_subdomains;
    }

    return thread_count;
}

void Manager::validate_config() const {
    auto drivers = impl_->app_ctx_->driver_manager_->get_loaded_drivers();
    auto interfaces = impl_->app_ctx_->network_manager_->get_interfaces();

    for (const auto &domain: impl_->config_.domains) {
        // check drivers
        if (std::ranges::find(drivers, domain.driver) == drivers.end()) {
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
    auto &driver_manager = impl_->app_ctx_->driver_manager_;

    auto &load = impl_->config_.driver.load;
    // remove duplicated lines
    impl_->dedupe(load);

    // load drivers
    auto base_dir = std::filesystem::path(impl_->config_.driver.driver_dir);
    for (auto &driver: load) {
        auto driver_full_path = base_dir.empty() ? std::filesystem::path(driver) : base_dir / driver;
        driver_manager->load_driver(driver_full_path.string());
    }
}

void Manager::create_worker() const {
    for (const auto &domain: impl_->config_.domains) {
        impl_->workers_.emplace_back(impl_->app_ctx_, domain, impl_->config_.resolver);
    }
}

void Manager::run(std::stop_token stop_token) const {
    // print all interfaces name
    auto interfaces = impl_->app_ctx_->network_manager_->get_interfaces();
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

    // create worker jthreads
    std::vector<std::jthread> worker_threads;
    worker_threads.reserve(impl_->workers_.size());
    for (auto &worker: impl_->workers_) {
        worker_threads.emplace_back(&Worker::run, &worker, stop_token);
    }

    // jthread destructors join automatically when vector goes out of scope.
    // If a stop is requested (e.g. via signal), each Worker::run will exit
    // its loop, and the jthread destructors will join them.
}

Manager::Manager(std::shared_ptr<AppContext> app_ctx, Config::config config)
    : impl_(std::make_unique<Impl>(std::move(app_ctx), std::move(config))) {
}

Manager::~Manager() = default;
