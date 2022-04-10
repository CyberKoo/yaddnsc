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
#include "context.h"
#include "ip_util.h"
#include "httpclient.h"

#include "exception/dns_lookup_exception.h"
#include "exception/driver_exception.h"

void Worker::run() {
    SPDLOG_INFO("Worker for domain {} started.", _worker_config.name);
    auto &context = Context::getInstance();

    std::mutex mutex;
    std::unique_lock<std::mutex> lock(mutex);
    auto update_interval = std::chrono::seconds(_worker_config.update_interval);

    while (!context.terminate) {
        auto updater = std::thread(&Worker::run_scheduled_tasks, this);
        updater.detach();
        context.cv.wait_for(lock, update_interval, [&context]() { return context.terminate; });
    }
}

std::optional<std::string> Worker::dns_lookup(std::string_view host, dns_record_t type) {
    auto &context = Context::getInstance();
    std::optional<DNS::dns_server_t> server = std::nullopt;

    if (context.resolver_config.use_customise_server) {
        server = {.ip_address = context.resolver_config.ip_address,
                .port = context.resolver_config.port};
    }

    try {
        return Util::retry_on_exception<std::string, DnsLookupException>(
                [&]() {
                    auto dns_answer = DNS::resolve(host, type, server);
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

void Worker::run_scheduled_tasks() {
    try {
        SPDLOG_DEBUG("---- Event loop start for {} ----", _worker_config.name);
        auto &context = Context::getInstance();
        auto &driver = context.driver_manager->get_driver(_worker_config.driver);
        bool force_update = is_forced_update();
        SPDLOG_DEBUG("Update counter: {}, estimated elapsed time {} seconds, force update: {}", _force_update_counter,
                     _force_update_counter * _worker_config.update_interval, force_update);

        for (const auto &sub_domain: _worker_config.subdomains) {
            auto fqdn = fmt::format("{}.{}", sub_domain.name, _worker_config.name);

            try {
                SPDLOG_DEBUG("**** Domain {} task start ****", fqdn);
                auto rd_type = magic_enum::enum_name(sub_domain.type);

                if (auto ip_addr = get_ip_address(sub_domain, force_update)) {
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

                        // this may throw exception
                        auto update_result = update_dns_record(request, sub_domain.ip_type, sub_domain.interface);
                        if (update_result.has_value()) {
                            if (driver->check_response(update_result.value())) {
                                SPDLOG_INFO("Update {}, type: {}, to {}", fqdn, rd_type, *ip_addr);
                            }
                        } else {
                            SPDLOG_INFO("Update domain {} failed", fqdn);
                        }

                        SPDLOG_DEBUG("**** Domain {} task finished ****", fqdn);
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
        SPDLOG_DEBUG("---- Event loop finished for {} ----", _worker_config.name);
    } catch (std::exception &e) {
        SPDLOG_CRITICAL("Scheduler exited with unhandled error: {}", e.what());
    }
}

std::optional<std::string> Worker::get_ip_address(const Config::sub_domain_config_t &config, bool bypass_cache) {
    auto ip_type = rdtype2ip(config.type);
    if (config.ip_source == Config::ip_source_t::INTERFACE) {
        auto addresses = IPUtil::get_ip_from_interface(config.interface, ip_type);
        if (!addresses.empty()) {
            return addresses.front();
        }
    } else {
        // ipv6 do not pass nif, this is a bug in cpp-httplib
        auto nif_name = ip_type == ip_version_t::IPV6 ? nullptr : config.interface.data();
        return IPUtil::get_ip_from_url(config.ip_source_param, ip_type, nif_name);
    }

    return std::nullopt;
}

std::optional<std::string>
Worker::update_dns_record(const request_t &request, ip_version_t version, std::string_view nif) {
    auto response = [&request, &nif, &version]() {
        auto uri = Uri::parse(request.url);
        auto path = HttpClient::build_request(uri);
        auto headers = httplib::Headers{};
        // copy headers
        headers.insert(request.header.begin(), request.header.end());

        // do not force interface when update ipv6 record
        auto nif_name = version == ip_version_t::IPV6 ? nullptr : nif.data();
        auto client = HttpClient::connect(uri, IPUtil::ip2af(version), nif_name);
        switch (request.request_method) {
            case request_method_t::GET:
                return client->Get(path.c_str(), headers);
            case request_method_t::POST:
                return client->Post(path.c_str(), headers, request.body, request.content_type.c_str());
            case request_method_t::PUT:
                return client->Put(path.c_str(), headers, request.body, request.content_type.c_str());
            default:
                return client->Get(path.c_str(), headers);
        }
    }();

    if (response) {
        return response->body;
    } else {
        SPDLOG_ERROR("HTTP request failed, error: {}", httplib::to_string(response.error()));
    }

    return std::nullopt;
}

bool Worker::is_forced_update() const {
    return (_worker_config.force_update > 0) &&
           (_force_update_counter * _worker_config.update_interval) > _worker_config.force_update;
}

ip_version_t Worker::rdtype2ip(dns_record_t type) {
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
