//
// Created by Kotarou on 2022/4/5.
//

#include "worker.h"

#include <thread>
#include <httplib.h>
#include <magic_enum.hpp>
#include <spdlog/spdlog.h>

#include "dns.h"
#include "uri.h"
#include "util.h"
#include "config.h"
#include "context.h"
#include "ip_util.h"
#include "IDriver.h"
#include "httpclient.h"
#include "driver_manager.h"

#include "exception/driver_exception.h"
#include "exception/dns_lookup_exception.h"

class Worker::Impl {
public:
    explicit Impl(const Config::domains_config_t &domain_config, const Config::resolver_config_t &resolver_config)
            : _dns_server(resolver_config.use_customise_server ?
                          std::make_optional<dns_server_t>({resolver_config.ip_address, resolver_config.port})
                                                               : std::nullopt),
              _worker_config(domain_config) {
    };

    ~Impl() = default;

public:

    void run_scheduled_tasks();

    [[nodiscard]] bool is_forced_update() const;

    std::optional<std::string> dns_lookup(std::string_view host, dns_record_t);

    std::optional<std::string> get_ip_address(const Config::sub_domain_config_t &);

    std::optional<std::string> update_dns_record(const driver_request_t &, ip_version_t, std::string_view);

    ip_version_t rdtype2ip(dns_record_t);

    const std::optional<dns_server_t> _dns_server;

    const Config::domains_config_t &_worker_config;

    int _force_update_counter = 0;
};

void Worker::run() {
    SPDLOG_INFO("Worker for domain {} started, update interval: {}s", _impl->_worker_config.name,
                _impl->_worker_config.update_interval);
    auto &context = Context::getInstance();

    std::mutex mutex;
    std::unique_lock<std::mutex> lock(mutex);
    auto update_interval = std::chrono::seconds(_impl->_worker_config.update_interval);

    while (!context.terminate) {
        auto updater = std::thread(&Impl::run_scheduled_tasks, _impl.get());
        updater.detach();
        context.cv.wait_for(lock, update_interval, [&context]() { return context.terminate; });
    }
}

std::optional<std::string> Worker::Impl::dns_lookup(std::string_view host, dns_record_t type) {
    try {
        return Util::retry_on_exception<std::string, DnsLookupException>(
                [&]() {
                    auto dns_answer = DNS::resolve(host, type, _dns_server);
                    if (dns_answer.size() > 1) {
                        SPDLOG_WARN("{} resolved more than one address (count: {})", host, dns_answer.size());
                    }

                    return dns_answer.front();
                }, 3,
                [](const DnsLookupException &e) {
                    return e.get_error() == dns_lookup_error_t::RETRY;
                }, 500
        );
    } catch (DnsLookupException &e) {
        SPDLOG_WARN("Resolve domain {} type: {} failed. Error: {}", host, magic_enum::enum_name(type),
                    DNS::error_to_str(e.get_error()));
    }

    return std::nullopt;
}

void Worker::Impl::run_scheduled_tasks() {
    try {
        auto &context = Context::getInstance();
        auto &driver = context.driver_manager->get_driver(_worker_config.driver);
        bool force_update = is_forced_update();
        SPDLOG_DEBUG("Update counter: {}, estimated elapsed time {} seconds, force update: {}", _force_update_counter,
                     _force_update_counter * _worker_config.update_interval, force_update);

        for (const auto &sub_domain: _worker_config.subdomains) {
            auto fqdn = fmt::format("{}.{}", sub_domain.name, _worker_config.name);

            try {
                auto rd_type = magic_enum::enum_name(sub_domain.type);

                if (auto ip_addr = get_ip_address(sub_domain)) {
                    auto record = dns_lookup(fqdn, sub_domain.type);
                    // force update or ip not same or even no ip
                    if (force_update || (record.has_value() && record.value() != *ip_addr) || !record.has_value()) {
                        if (force_update) {
                            SPDLOG_INFO("Force update triggered!!");
                            _force_update_counter = 0;
                        }

                        auto parameters = std::map<std::string, std::string>(sub_domain.driver_param);
                        parameters.emplace("domain", _worker_config.name);
                        parameters.emplace("subdomain", sub_domain.name);
                        parameters.emplace("ip_addr", *ip_addr);
                        parameters.emplace("rd_type", rd_type);
                        parameters.emplace("fqdn", fqdn);

                        auto request = driver->generate_request(parameters);
                        SPDLOG_DEBUG("Received DNS record update instruction from driver {}",
                                     driver->get_detail().name);

                        // update dns record via http request
                        auto update_result = update_dns_record(request, sub_domain.ip_type, sub_domain.interface);
                        if (update_result.has_value() && driver->check_response(*update_result)) {
                            SPDLOG_INFO("Update {}, type: {}, to {}", fqdn, rd_type, *ip_addr);
                        } else {
                            SPDLOG_WARN("Update domain {} failed", fqdn);
                        }
                    } else {
                        SPDLOG_DEBUG("Domain: {}, type: {}, current {}, new {}, skip updating", fqdn, rd_type,
                                     (record.has_value() ? *record : "<empty>"), *ip_addr);
                    }
                } else {
                    SPDLOG_WARN("No valid IP Address found, skip update");
                }
            } catch (DriverException &e) {
                SPDLOG_ERROR("Task for domain {}, ended with driver exception: {}", fqdn, e.what());
            }
        }

        ++_force_update_counter;
    } catch (std::exception &e) {
        SPDLOG_CRITICAL("Scheduler exited with unhandled error: {}", e.what());
    }
}

std::optional<std::string> Worker::Impl::get_ip_address(const Config::sub_domain_config_t &config) {
    auto ip_type = rdtype2ip(config.type);
    if (config.ip_source == Config::ip_source_t::INTERFACE) {
        auto addresses = IPUtil::get_ip_from_interface(config.interface, ip_type);
        if (!addresses.empty()) {
            return addresses.front();
        }
    } else {
        return IPUtil::get_ip_from_url(config.ip_source_param, ip_type, config.interface.data());
    }

    return std::nullopt;
}

std::optional<std::string>
Worker::Impl::update_dns_record(const driver_request_t &request, ip_version_t version, std::string_view nif) {
    auto response = [&request, &nif, &version]() {
        auto uri = Uri::parse(request.url);
        auto path = HttpClient::build_request(uri);
        auto headers = httplib::Headers{};
        // copy headers
        headers.insert(request.header.begin(), request.header.end());

        auto client = HttpClient::connect(uri, IPUtil::ip2af(version), nif.data());
        switch (request.request_method) {
            case driver_http_method_t::GET:
                return client.Get(path.c_str(), headers);
            case driver_http_method_t::POST:
                return std::visit([&](const auto &body) {
                                      using T = std::decay_t<decltype(body)>;
                                      if constexpr (std::is_same_v<T, driver_param_t>)
                                          return client.Post(path.c_str(), headers, body);
                                      else if constexpr (std::is_same_v<T, std::string>)
                                          return client.Post(path.c_str(), headers, body, request.content_type.c_str());
                                  }, request.body
                );
            case driver_http_method_t::PUT:
                return std::visit([&](const auto &body) {
                                      using T = std::decay_t<decltype(body)>;
                                      if constexpr (std::is_same_v<T, driver_param_t>)
                                          return client.Put(path.c_str(), headers, body);
                                      else if constexpr (std::is_same_v<T, std::string>)
                                          return client.Put(path.c_str(), headers, body, request.content_type.c_str());
                                  }, request.body
                );
            default:
                return client.Get(path.c_str(), headers);
        }
    }();

    if (response) {
        return response->body;
    } else {
        SPDLOG_ERROR("HTTP request failed, error: {}", httplib::to_string(response.error()));
    }

    return std::nullopt;
}

bool Worker::Impl::is_forced_update() const {
    return (_worker_config.force_update >= _worker_config.update_interval) &&
           (_force_update_counter * _worker_config.update_interval) > _worker_config.force_update;
}

ip_version_t Worker::Impl::rdtype2ip(dns_record_t type) {
    switch (type) {
        case dns_record_t::A:
            return ip_version_t::IPV4;
        case dns_record_t::AAAA:
            return ip_version_t::IPV6;
        case dns_record_t::TXT:
            return ip_version_t::UNSPECIFIED;
        default:
            return ip_version_t::UNSPECIFIED;
    }
}

Worker::Worker(const Config::domains_config_t &domain_config, const Config::resolver_config_t &resolver_config) : _impl(
        new Worker::Impl(domain_config, resolver_config)) {
}

void Worker::ImplDeleter::operator()(Worker::Impl *ptr) {
    delete ptr;
}
