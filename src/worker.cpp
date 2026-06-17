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
#include "app_context.h"
#include "ip_util.h"
#include "driver_interface.h"
#include "http_client.h"
#include "driver_manager.h"

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

    [[nodiscard]] bool is_forced_update() const;

    [[nodiscard]] std::optional<std::string> dns_lookup(const std::string &, dns_type) const;

    static std::optional<std::string> get_ip_address(NetworkManager &, const Config::subdomain_config &);

    static std::optional<std::string> update_dns_record(const driver_request &, address_family, const std::string &);

    static httplib::Result do_http_request(const driver_request &, address_family, const std::string &);

    static address_family dns2ip(dns_type);

    static std::string_view to_string(dns_type);

    static BS::thread_pool<> &get_thread_pool();

    static bool is_ipv6_la(const std::string &);

    static bool is_ipv6_ula(const std::string &);

    static bool is_ipv6_local_link(const in6_addr *);

    static bool is_ipv6_site_local(const in6_addr *);

    static bool is_ipv6_unique_local(const in6_addr *);

    static inline std::optional<dns_server> get_dns_server(const Config::resolver_config &) noexcept;

public:
    std::shared_ptr<AppContext> app_ctx_;

    const std::optional<dns_server> dns_server_;

    Config::domain_config worker_config_;

    long force_update_counter_ = 0;

    static constexpr int RESOLVER_RETRY = 5;

    static constexpr int RESOLVER_RETRY_BACKOFF = 1000;
};

#ifdef YADDNSC_USE_STD_FORMAT
template<>
struct std::formatter<driver_request> {
#else
    template<>
    struct fmt::formatter<driver_request> {



#endif
    static std::string_view to_string(driver_http_method_type type) {
        switch (type) {
            case driver_http_method_type::GET:
                return "GET";
            case driver_http_method_type::POST:
                return "POST";
            case driver_http_method_type::PUT:
                return "PUT";
            case driver_http_method_type::PATCH:
                return "PATCH";
            case driver_http_method_type::DELETE:
                return "DELETE";
            default:
                return "UNKNOWN";
        }
    }

    template<typename Iter>
#ifdef YADDNSC_USE_STD_FORMAT
    [[nodiscard]] std::string format_map(Iter first, Iter last) const {
#else
        [[nodiscard]] std::string format_map(Iter first, Iter last) const {
#endif
        std::string buf;

        for (auto begin = first, it = begin, end = last; it != end; ++it) {
            buf.append(it->first);
            buf.append("=");
            buf.append(it->second);
            buf.append("; ");
        }

        if (!buf.empty() && buf.size() >= 2) {
            buf.erase(buf.end() - 2);
        }

        return buf;
    }

    static constexpr auto parse(format_parse_context &ctx) -> decltype(ctx.begin()) {
        return ctx.begin();
    }

    template<typename FormatContext>
#ifdef YADDNSC_USE_STD_FORMAT
    auto format(const driver_request &request, FormatContext &ctx) const -> decltype(ctx.out()) {
#else
        auto format(const driver_request &request, FormatContext &ctx) const -> decltype(ctx.out()) {
#endif
        std::string body_type;
        std::string body;

        std::visit([&]<typename T>(const T &body_) {
                       if constexpr (std::is_same_v<T, driver_param_type>) {
                           body_type = "map";
                           body = format_map(body_.begin(), body_.end());
                       } else if constexpr (std::is_same_v<T, std::string>) {
                           body_type = "string";
                           body = body_;
                       }
                   }, request.body
        );

#ifdef YADDNSC_USE_STD_FORMAT
        return std::format_to(ctx.out(),
                              R"(driver_request(url="{}", body_type="{}", body="{}", content_type="{}", request_method="{}", header="{}"))",
                              request.url, body_type, body, request.content_type, to_string(request.request_method),
                              format_map(request.header.begin(), request.header.end()));
#else
        return fmt::format_to(ctx.out(),
                              R"(driver_request(url="{}", body_type="{}", body="{}", content_type="{}", request_method="{}", header="{}"))",
                              request.url, body_type, body, request.content_type, to_string(request.request_method),
                              format_map(request.header.begin(), request.header.end()));
#endif
    }
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
        SPDLOG_WARN("DNS lookup for domain {} type: {} failed after {} retries. Error: {}", host, to_string(type),
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
            auto fqdn = fmt::format("{}.{}", config.name, worker_config_.name);

            try {
                auto rd_type = to_string(config.type);

                if (auto ip_addr = get_ip_address(*app_ctx_->network_manager_, config)) {
                    auto record = dns_lookup(fqdn, config.type);
                    auto record_val = record.value_or("<empty>");
                    auto is_record_staled = record.has_value() && record.value() != *ip_addr;
                    // force update or ip not same or even no ip
                    if (force_update || is_record_staled || !record.has_value()) {
                        if (force_update) {
                            SPDLOG_INFO("Force update triggered");
                            force_update_counter_ = 0;
                        }

                        auto parameters = driver_config_type{config.driver_param};
                        parameters.try_emplace("domain", worker_config_.name);
                        parameters.try_emplace("subdomain", config.name);
                        parameters.try_emplace("ip_addr", *ip_addr);
                        parameters.try_emplace("rd_type", rd_type);
                        parameters.try_emplace("fqdn", fqdn);

                        // if ip not equal print log
                        if (is_record_staled) {
                            SPDLOG_INFO(R"(Update needed, local IP "{}" != DNS record "{}")", *ip_addr, record_val);
                        }

                        auto request = driver.generate_request(parameters);
                        SPDLOG_DEBUG("Received DNS record update request from driver {}, {}", driver.get_detail().name,
                                     request);

                        // update dns record via http request
                        auto update_result = update_dns_record(request, config.ip_type, config.interface);
                        if (update_result.has_value() && driver.check_response(*update_result)) {
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
            } catch (DriverException &e) {
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

std::optional<std::string>
Worker::Impl::get_ip_address(NetworkManager &net_mgr, const Config::subdomain_config &config) {
    auto ip_type = dns2ip(config.type);
    if (config.ip_source == Config::ip_source_type::INTERFACE) {
        auto addresses = IPUtil::get_ip_from_interface(net_mgr, config.interface, ip_type);

        if (ip_type == address_family::IPV6) {
            // filter out local link
            if (!config.allow_local_link) {
                std::erase_if(addresses, &is_ipv6_la);
            }

            // filter out ula
            if (!config.allow_ula) {
                std::erase_if(addresses, &is_ipv6_ula);
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

httplib::Result
Worker::Impl::do_http_request(const driver_request &request, address_family version, const std::string &nif) {
    const auto uri = Uri::parse(request.url);
    const auto path = HttpClient::build_request(uri);
    auto client = HttpClient::connect(uri, IPUtil::ip2af(version), nif.c_str());
    httplib::Headers headers{request.header.begin(), request.header.end()};

    auto with_body = [&](auto method) -> httplib::Result {
        if (auto *body = std::get_if<driver_param_type>(&request.body)) {
            return method(path.c_str(), std::move(headers), *body);
        }

        return method(path.c_str(), std::move(headers),
                      std::get<std::string>(request.body), request.content_type.c_str());
    };

    switch (request.request_method) {
        case driver_http_method_type::GET:
            return client.Get(path, headers);
        case driver_http_method_type::POST:
            return with_body([&]<typename... T>(T &&... args) { return client.Post(std::forward<T>(args)...); });
        case driver_http_method_type::PUT:
            return with_body([&]<typename... T>(T &&... args) { return client.Put(std::forward<T>(args)...); });
        case driver_http_method_type::PATCH:
            return with_body([&]<typename... T>(T &&... args) { return client.Patch(std::forward<T>(args)...); });
        case driver_http_method_type::DELETE:
            return client.Delete(path, headers);
        default:
            return client.Get(path, headers);
    }
}

std::optional<std::string>
Worker::Impl::update_dns_record(const driver_request &request, address_family version, const std::string &nif) {
    auto response = do_http_request(request, version, nif);

    if (response) {
        return response->body;
    }

    SPDLOG_ERROR("HTTP request failed, error: {}", httplib::to_string(response.error()));
    return std::nullopt;
}

bool Worker::Impl::is_forced_update() const {
    return (worker_config_.force_update >= worker_config_.update_interval) &&
           (force_update_counter_ * worker_config_.update_interval) >= worker_config_.force_update;
}

address_family Worker::Impl::dns2ip(dns_type type) {
    switch (type) {
        case dns_type::A:
            return address_family::IPV4;
        case dns_type::AAAA:
            return address_family::IPV6;
        default:
            return address_family::UNSPECIFIED;
    }
}

std::string_view Worker::Impl::to_string(dns_type type) {
    switch (type) {
        case dns_type::A:
            return "A";
        case dns_type::AAAA:
            return "AAAA";
        case dns_type::TXT:
            return "TXT";
        case dns_type::SOA:
            return "SOA";
        default:
            return "UNKNOWN";
    }
}

BS::thread_pool<> &Worker::Impl::get_thread_pool() {
    static BS::thread_pool thread_pool{};

    return thread_pool;
}

bool Worker::Impl::is_ipv6_local_link(const in6_addr *addr) {
    return addr->s6_addr[0] == 0xfe && (addr->s6_addr[1] & 0xc0) == 0x80;
}

bool Worker::Impl::is_ipv6_site_local(const in6_addr *addr) {
    return addr->s6_addr[0] == 0xfe && (addr->s6_addr[1] & 0xc0) == 0xc0;
}

bool Worker::Impl::is_ipv6_unique_local(const in6_addr *addr) {
    return addr->s6_addr[0] == 0xfc || addr->s6_addr[0] == 0xfd;
}

bool Worker::Impl::is_ipv6_ula(const std::string &ip_addr) {
    sockaddr_in6 sa{};
    if (inet_pton(AF_INET6, ip_addr.data(), &(sa.sin6_addr)) != 1) {
        return false;
    }
    return is_ipv6_unique_local(&sa.sin6_addr);
}

bool Worker::Impl::is_ipv6_la(const std::string &ip_addr) {
    sockaddr_in6 sa{};
    if (inet_pton(AF_INET6, ip_addr.data(), &sa.sin6_addr) != 1) {
        return true; // unparseable → treat as link-local (don't use)
    }
    return is_ipv6_local_link(&sa.sin6_addr) || is_ipv6_site_local(&sa.sin6_addr);
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
