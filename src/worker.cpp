//
// Created by Kotarou on 2022/4/5.
//

#include "worker.h"

#include <thread>
#include <climits>
#include <utility>
#include <condition_variable>

#include "fmt.h"
#include "stop_token_compat.h"

#include <httplib.h>
#include <spdlog/spdlog.h>
#include <BS_thread_pool.hpp>

#include "dns.h"
#include "uri.h"
#include "util.h"
#include "config.h"
#include "ip_util.h"
#include "app_context.h"
#include "http_client.h"
#include "driver_manager.h"
#include "driver_interface.h"
#include "http_type_formatter.hpp"

#include "exception/driver_exception.h"
#include "exception/dns_lookup_exception.h"

class Worker::Impl {
public:
    explicit Impl(std::shared_ptr<AppContext> app_ctx,
                  Config::domain_config domain_config,
                  const Config::resolver_config &resolver_config)
        : app_ctx_(std::move(app_ctx)),
          dns_server_(get_dns_server(resolver_config)),
          worker_config_(std::move(domain_config)) {
    }

    ~Impl() = default;

    void run_scheduled_tasks();

    void process_subdomain(const Config::subdomain_config &, class IDriver &, bool force_update);

    [[nodiscard]] driver_config_type
    build_driver_parameters(const Config::subdomain_config &config,
                            const std::string &ip_addr,
                            std::string_view rd_type,
                            std::string_view fqdn);

    [[nodiscard]] bool is_forced_update() const;

    [[nodiscard]] std::optional<std::string> dns_lookup(const std::string &, dns_type) const;

    [[nodiscard]] std::optional<std::string> get_ip_address(const Config::subdomain_config &) const;

    static BS::thread_pool<> &get_thread_pool();

    static inline std::optional<dns_server> get_dns_server(const Config::resolver_config &) noexcept;

public:
    std::shared_ptr<AppContext> app_ctx_;

    const std::optional<dns_server> dns_server_;

    Config::domain_config worker_config_;

    long force_update_counter_ = 0;

    static constexpr int RESOLVER_RETRY = 5;

    static constexpr int RESOLVER_RETRY_BACKOFF = 1000;
};

void Worker::run(std::stop_token st) const {
    SPDLOG_INFO(R"(Worker for domain "{}" started, update interval: {}s)", impl_->worker_config_.name,
                impl_->worker_config_.update_interval);
    auto &thread_pool = Impl::get_thread_pool();
    auto update_interval = std::chrono::seconds(impl_->worker_config_.update_interval);

    std::mutex cv_mtx;
    std::condition_variable cv;
    // Register a callback that notifies the cv when stop is requested,
    // so the worker wakes up immediately instead of blocking for the full interval.
    std::stop_callback cb(st, [&cv] { cv.notify_all(); });

    while (!st.stop_requested()) {
        thread_pool.detach_task([_impl_ptr = impl_.get()] { _impl_ptr->run_scheduled_tasks(); });

        std::unique_lock lock(cv_mtx);
        cv.wait_for(lock, update_interval, [&st] { return st.stop_requested(); });
    }

    SPDLOG_INFO(R"(Worker for domain "{}" stopped)", impl_->worker_config_.name);
}

std::optional<std::string> Worker::Impl::dns_lookup(const std::string &host, dns_type type) const {
    try {
        return Util::retry_on_exception<std::string, DnsLookupException>(
            [&] {
                auto dns_answer = DNS::resolve(host, type, dns_server_);
                if (dns_answer.size() > 1) {
                    SPDLOG_WARN(R"(Domain "{}" resolved to more than one address (count: {}))", host,
                                dns_answer.size());
                }

                return dns_answer.front();
            }, RESOLVER_RETRY,
            [](const DnsLookupException &e) {
                return e.get_error() == dns_error::RETRY;
            }, RESOLVER_RETRY_BACKOFF
        );
    } catch (DnsLookupException &e) {
        SPDLOG_WARN("DNS lookup for domain {} type: {} failed after {} retries. Error: {}", host, DNS::to_string(type),
                    RESOLVER_RETRY, DNS::error_to_str(e.get_error()));
    }

    return std::nullopt;
}

void Worker::Impl::run_scheduled_tasks() {
    try {
        auto &driver = app_ctx_->driver_manager_->get_driver(worker_config_.driver);
        bool force_update = is_forced_update();
        SPDLOG_DEBUG("Update counter: {}, estimated elapsed time {} seconds, force update: {}", force_update_counter_,
                     force_update_counter_ * worker_config_.update_interval, force_update);

        for (const auto &config: worker_config_.subdomains) {
            try {
                process_subdomain(config, driver, force_update);
            } catch (DriverException &e) {
                auto fqdn = fmt::format("{}.{}", config.name, worker_config_.name);
                SPDLOG_ERROR("Task for domain {} ended with a driver exception: {}", fqdn, e.what());
            }
        }

        if (worker_config_.force_update > 0 && force_update_counter_ < LONG_MAX) {
            ++force_update_counter_;
        }
    } catch (std::exception &e) {
        SPDLOG_CRITICAL("Scheduler exited with an unhandled error: {}", e.what());
    }
}

void Worker::Impl::process_subdomain(const Config::subdomain_config &config,
                                     IDriver &driver,
                                     bool force_update) {
    auto fqdn = fmt::format("{}.{}", config.name, worker_config_.name);
    auto rd_type = DNS::to_string(config.type);

    if (auto ip_addr = get_ip_address(config)) {
        auto record = dns_lookup(fqdn, config.type);
        auto record_val = record.value_or("<empty>");
        auto is_record_staled = record.has_value() && record.value() != *ip_addr;

        if (force_update || is_record_staled || !record.has_value()) {
            if (force_update) {
                SPDLOG_INFO("Force update triggered");
                force_update_counter_ = 0;
            }

            auto parameters = build_driver_parameters(config, *ip_addr, rd_type, fqdn);

            if (is_record_staled) {
                SPDLOG_INFO(R"(Update needed, local IP "{}" != DNS record "{}")", *ip_addr, record_val);
            }

            auto request = driver.generate_request(parameters);
            SPDLOG_DEBUG("Received DNS record update request from driver {}, {}", driver.get_detail().name, request);

            auto response = HttpClient::send(request, config.ip_type, config.interface);
            if (response && driver.check_response(response->body)) {
                SPDLOG_INFO("Update {}, type: {}, to {}", fqdn, rd_type, *ip_addr);
            } else {
                SPDLOG_WARN("Update domain {} failed", fqdn);
            }
        } else {
            SPDLOG_DEBUG("Domain: {}, type: {}, current {}, new {}, skipping update", fqdn, rd_type,
                         record_val, *ip_addr);
        }
    } else {
        SPDLOG_WARN("No valid IP address found, skipping the update");
    }
}

driver_config_type Worker::Impl::build_driver_parameters(
    const Config::subdomain_config &config,
    const std::string &ip_addr,
    std::string_view rd_type,
    std::string_view fqdn) {
    auto parameters = driver_config_type{config.driver_param};
    parameters.try_emplace("domain", worker_config_.name);
    parameters.try_emplace("subdomain", config.name);
    parameters.try_emplace("ip_addr", ip_addr);
    parameters.try_emplace("rd_type", rd_type);
    parameters.try_emplace("fqdn", fqdn);
    return parameters;
}

std::optional<std::string> Worker::Impl::get_ip_address(const Config::subdomain_config &config) const {
    auto ip_type = DNS::dns2ip(config.type);
    if (config.ip_source == Config::ip_source_type::INTERFACE) {
        auto addresses = IPUtil::get_ip_from_interface(*app_ctx_->network_manager_, config.interface, ip_type);

        if (ip_type == address_family::IPV6) {
            // filter out local link
            if (!config.allow_local_link) {
                std::erase_if(addresses, &IPUtil::is_ipv6_local_link);
            }

            // filter out ula
            if (!config.allow_ula) {
                std::erase_if(addresses, &IPUtil::is_ipv6_ula);
            }
        }

        if (!addresses.empty()) {
            return addresses.front();
        }
    } else {
        return IPUtil::get_ip_from_url(config.ip_source_param, ip_type, config.interface.data());
    }

    return std::nullopt;
}

bool Worker::Impl::is_forced_update() const {
    return (worker_config_.force_update >= worker_config_.update_interval) &&
           (force_update_counter_ * worker_config_.update_interval) >= worker_config_.force_update;
}



BS::thread_pool<> &Worker::Impl::get_thread_pool() {
    static BS::thread_pool thread_pool{};

    return thread_pool;
}

std::optional<dns_server> Worker::Impl::get_dns_server(const Config::resolver_config &config) noexcept {
    if (config.use_custom_server) {
        return std::make_optional<dns_server>({config.ip_address, config.port});
    }

    return std::nullopt;
}

void Worker::set_concurrency(unsigned int thread_count) {
    SPDLOG_INFO("Set worker thread-pool size to {}", thread_count);
    if (Impl::get_thread_pool().get_thread_count() != thread_count) {
        Impl::get_thread_pool().reset(thread_count);
    }
}

Worker::Worker(std::shared_ptr<AppContext> app_ctx,
               const Config::domain_config &domain_config,
               const Config::resolver_config &resolver_config)
    : impl_(std::make_unique<Impl>(std::move(app_ctx), domain_config, resolver_config)) {
}

Worker::~Worker() = default;

Worker::Worker(Worker &&) noexcept = default;
