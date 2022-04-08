//
// Created by Kotarou on 2022/4/5.
//

#include <thread>
#include <httplib.h>
#include <spdlog/spdlog.h>

#include "dns.h"
#include "uri.h"
#include "worker.h"
#include "context.h"
#include "ip_util.h"
#include "httpclient.h"
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
    try {
        std::optional<DNS::dns_server_t> server = std::nullopt;
        if (context.resolver_config.use_customise_server) {
            server = {.ip_address = context.resolver_config.ip_address,
                    .port = context.resolver_config.port};
        }

        auto dns_answer = DNS::resolve(host, type, server);

        if (dns_answer.size() > 1) {
            SPDLOG_WARN("{} resolved more than one address (count: {})", host, dns_answer.size());
        }

        return dns_answer.front();
    } catch (std::exception &e) {
        SPDLOG_WARN("Resolve domain {} type: {} failed. Error: {}", host, record_type_to_string(type), e.what());
        return std::nullopt;
    }
}

void Worker::run_scheduled_tasks() {
    try {
        SPDLOG_DEBUG("---- Event loop start ----");
        auto &context = Context::getInstance();
        auto &driver = context.driver_manager->get_driver(_worker_config.driver);
        bool force_update = (_force_update_counter * _worker_config.update_interval) > _worker_config.force_update;
        SPDLOG_DEBUG("Update counter: {}, estimated elapsed time {} seconds, force update: {}", _force_update_counter,
                     _force_update_counter * _worker_config.update_interval, force_update);

        for (const auto &sub_domain: _worker_config.subdomains) {
            auto fqdn = fmt::format("{}.{}", sub_domain.name, _worker_config.name);

            SPDLOG_DEBUG("**** Domain {} task start ****", fqdn);
            auto rd_type = record_type_to_string(sub_domain.type);

            if (auto ip_addr = get_ip_address(sub_domain)) {
                auto record = dns_lookup(fqdn, sub_domain.type);

                if (!record.has_value()) {
                    SPDLOG_WARN("DNS lookup did not return any value, proceed anyway.");
                }

                if (record->empty() || record.value() != *ip_addr || force_update) {
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
                    SPDLOG_DEBUG("Received DNS record update instruction from driver {}", driver->get_detail().name);

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
                                 (record->empty() ? "<empty>" : record.value()), *ip_addr);
                }
            } else {
                SPDLOG_WARN("No valid IP Address found, skip update");
            }
        }
        ++_force_update_counter;
        SPDLOG_DEBUG("---- Event loop finished ----");
    } catch (std::exception &e) {
        SPDLOG_CRITICAL("Scheduler exited with unhandled error: {}", e.what());
    }
}

std::optional<std::string> Worker::get_ip_address(const Config::sub_domain_config_t &config) {
    if (config.ip_source == Config::ip_source_t::INTERFACE) {
        auto addresses = IPUtil::get_ip_from_interface(config.interface, record_type_to_ip_ver(config.type));
        if (!addresses.empty()) {
            return addresses.front();
        }
    } else {
        auto ip_ver = record_type_to_ip_ver(config.type);
        // ipv6 do not pass nif, this is a bug in cpp-httplib
        auto nif_name = ip_ver == ip_version_t::IPV6 ? nullptr : config.interface.data();
        return IPUtil::get_ip_from_url(config.ip_source_param, ip_ver, nif_name);
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
        auto client = HttpClient::connect(uri, IPUtil::ip_version_to_af(version), nif_name);
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

ip_version_t Worker::record_type_to_ip_ver(dns_record_t type) {
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

std::string Worker::record_type_to_string(dns_record_t type) {
    switch (type) {
        case dns_record_t::A:
            return "A";
        case dns_record_t::AAAA:
            return "AAAA";
        case dns_record_t::TXT:
            return "TXT";
        default:
            return "UNKNOWN";
    }
}
