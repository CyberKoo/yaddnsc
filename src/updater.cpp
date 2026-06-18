//
// Created by Kotarou on 2026/6/18.
//

#include "updater.h"

#include <utility>

#include "fmt.h"

#include <spdlog/spdlog.h>

#include <httplib.h>
#include <magic_enum/magic_enum.hpp>

#include "dns.h"
#include "util.h"
#include "ip_util.h"
#include "http_client.h"
#include "driver_manager.h"
#include "network_manager.h"
#include "driver_interface.h"
#include "http_type_formatter.hpp"

#include "exception/dns_lookup_exception.h"

// ---------------------------------------------------------------------------
// Updater::Impl — all private helpers live here.
// ---------------------------------------------------------------------------
class Updater::Impl {
public:
    explicit Impl(DriverManager &driver_manager,
                  NetworkManager &network_manager,
                  std::optional<dns_server> dns_server)
        : driver_manager_(driver_manager),
          network_manager_(network_manager),
          dns_server_(std::move(dns_server)) {
    }

    void process(const UpdateTask &task) const;

private:
    [[nodiscard]] std::optional<std::string>
    dns_lookup(const std::string &host, dns_type type) const;

    [[nodiscard]] std::optional<std::string>
    get_ip_address(const Config::subdomain_config &config, address_family af) const;

    [[nodiscard]] static driver_config_type
    build_driver_parameters(const UpdateTask &task, const std::string &ip_addr, std::string_view rd_type);

private:
    DriverManager &driver_manager_;
    NetworkManager &network_manager_;
    const std::optional<dns_server> dns_server_;

    static constexpr int RESOLVER_RETRY = 5;
    static constexpr int RESOLVER_RETRY_BACKOFF = 1000;
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Updater::Updater(DriverManager &driver_manager,
                 NetworkManager &network_manager,
                 std::optional<dns_server> dns_server)
    : impl_(std::make_unique<Impl>(driver_manager, network_manager, std::move(dns_server))) {
}

Updater::~Updater() = default;

void Updater::process(const UpdateTask &task) const {
    impl_->process(task);
}

// ---------------------------------------------------------------------------
// process() — the full update pipeline for one (sub)domain.
// ---------------------------------------------------------------------------
void Updater::Impl::process(const UpdateTask &task) const {
    auto rd_type_name = magic_enum::enum_name(task.subdomain.type);
    const auto rd_type = rd_type_name.empty() ? "UNKNOWN" : rd_type_name;
    const auto ip_type = DNS::dns2ip(task.subdomain.type);

    // --- Step 1: local IP ---------------------------------------------------

    const auto local_ip = get_ip_address(task.subdomain, ip_type);
    if (!local_ip) {
        SPDLOG_WARN("No valid IP address found for {}, skipping the update", task.fqdn);
        return;
    }

    // --- Step 2: DNS lookup ------------------------------------------------

    const auto record = dns_lookup(task.fqdn, task.subdomain.type);
    const auto record_val = record.value_or("<empty>");

    // --- Step 3: skip if unchanged ------------------------------------------

    if (!task.force_update && record.has_value() && record.value() == *local_ip) {
        SPDLOG_DEBUG("Domain: {}, type: {}, current {}, new {}, skipping update",
                     task.fqdn, rd_type, record_val, *local_ip);
        return;
    }

    if (record.has_value() && record.value() != *local_ip) {
        SPDLOG_INFO(R"(Update needed, local IP "{}" != DNS record "{}")", *local_ip, record_val);
    }

    if (task.force_update) {
        SPDLOG_INFO("Force update triggered for {}", task.fqdn);
    }

    // --- Step 4: build parameters & generate request ------------------------

    const auto parameters = build_driver_parameters(task, *local_ip, rd_type);

    IDriver *driver = nullptr;
    try {
        driver = &driver_manager_.get_driver(task.driver_name);
    } catch (const YaddnscException &e) {
        SPDLOG_ERROR("Driver '{}' not found for {}: {}", task.driver_name, task.fqdn, e.what());
        return;
    }

    const auto request = driver->generate_request(parameters);
    SPDLOG_DEBUG("Received DNS record update request from driver {}, {}",
                 driver->get_detail().name, request);

    // --- Step 5: send HTTP request ------------------------------------------

    const auto response = HttpClient::send(request, task.subdomain.ip_type, task.subdomain.interface);
    if (!response) {
        SPDLOG_WARN("Update for {} failed (HTTP {} error)", task.fqdn, magic_enum::enum_name(response.error()));
        return;
    }

    // --- Step 6: verify response --------------------------------------------

    if (!driver->check_response(response->body)) {
        SPDLOG_WARN("Update domain {} failed (driver rejected the response)", task.fqdn);
        return;
    }

    SPDLOG_INFO("Update {}, type: {}, to {}", task.fqdn, rd_type, *local_ip);
}

// ---------------------------------------------------------------------------
// dns_lookup — resolve a DNS name with retries.
// ---------------------------------------------------------------------------
std::optional<std::string>
Updater::Impl::dns_lookup(const std::string &host, dns_type type) const {
    try {
        return Util::retry_on_exception<std::string, DnsLookupException>(
            [&] {
                const auto dns_answer = DNS::resolve(host, type, dns_server_);
                if (dns_answer.empty()) {
                    throw DnsLookupException(
                        fmt::format(R"(DNS lookup for domain "{}" returned no records)", host),
                        dns_error::NODATA
                    );
                }

                if (dns_answer.size() > 1) {
                    SPDLOG_WARN(R"(Domain "{}" resolved to more than one address (count: {}))",
                                host, dns_answer.size());
                }

                return dns_answer.front();
            },
            RESOLVER_RETRY,
            [](const DnsLookupException &e) {
                return e.get_error() == dns_error::RETRY;
            },
            RESOLVER_RETRY_BACKOFF
        );
    } catch (const DnsLookupException &e) {
        SPDLOG_WARN("DNS lookup for domain {} type: {} failed after {} retries. Error: {}",
                    host, magic_enum::enum_name(type), RESOLVER_RETRY, DNS::error_to_str(e.get_error()));
    }

    return std::nullopt;
}

// ---------------------------------------------------------------------------
// get_ip_address — obtain the local IP from an interface or a URL.
// ---------------------------------------------------------------------------
std::optional<std::string>
Updater::Impl::get_ip_address(const Config::subdomain_config &config, address_family af) const {
    if (config.ip_source == Config::ip_source_type::INTERFACE) {
        const auto if_addresses = network_manager_.get_interface_ip_addresses(config.interface);
        auto addresses = IPUtil::extract_address(if_addresses, af);

        if (af == address_family::IPV6) {
            // Filter out link-local addresses unless explicitly allowed.
            if (!config.allow_local_link) {
                std::erase_if(addresses, &IPUtil::is_ipv6_local_link);
            }

            // Filter out unique-local addresses unless explicitly allowed.
            if (!config.allow_ula) {
                std::erase_if(addresses, &IPUtil::is_ipv6_ula);
            }
        }

        if (!addresses.empty()) {
            return addresses.front();
        }
    } else {
        return IPUtil::get_ip_from_url(config.ip_source_param, af, config.interface.data());
    }

    return std::nullopt;
}

// ---------------------------------------------------------------------------
// build_driver_parameters — merge subdomain.driver_param with the standard
//                           key-value pairs the driver expects.
// ---------------------------------------------------------------------------
driver_config_type
Updater::Impl::build_driver_parameters(const UpdateTask &task,
                                       const std::string &ip_addr,
                                       std::string_view rd_type) {
    auto parameters = driver_config_type{task.subdomain.driver_param};
    parameters.try_emplace("domain", task.domain_name);
    parameters.try_emplace("subdomain", task.subdomain.name);
    parameters.try_emplace("ip_addr", ip_addr);
    parameters.try_emplace("rd_type", rd_type);
    parameters.try_emplace("fqdn", task.fqdn);
    return parameters;
}
