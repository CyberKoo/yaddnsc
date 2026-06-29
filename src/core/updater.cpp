//
// Created by Kotarou on 2026/6/18.
//

#include "updater.h"

#include <spdlog/spdlog.h>
#include <magic_enum/magic_enum.hpp>

#include "dns/types.h"
#include "dns/multi_resolver.h"
#include "driver_manager.h"
#include "interfaces/driver.h"
#include "interfaces/http_client.h"
#include "network/http_client.h"
#include "network/inet_address.h"
#include "string_util.h"
#include "network/network_manager.h"

// ---------------------------------------------------------------------------
// Updater::Impl — all private helpers live here.
// ---------------------------------------------------------------------------
class Updater::Impl {
public:
    explicit Impl(const DriverManager &driver_manager, const NetworkManager &network_manager,
                  const MultiResolver &resolver_pool) : driver_manager_(driver_manager),
                                                        network_manager_(network_manager),
                                                        resolver_pool_(resolver_pool) {
    }

    void process(const UpdateTask &task) const;

private:
    [[nodiscard]] std::optional<std::string>
    dns_lookup(const std::string &host, dns_type type) const;

    [[nodiscard]] std::optional<InetAddress> get_ip_address(const Config::subdomain_config &config) const;

    [[nodiscard]] static driver_config_type
    build_driver_parameters(const UpdateTask &task);

    [[nodiscard]] static UpdateContext
    build_update_context(const UpdateTask &task, const InetAddress &ip_addr, std::string_view rd_type);

private:
    const DriverManager &driver_manager_;
    const NetworkManager &network_manager_;
    const MultiResolver &resolver_pool_;
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Updater::Updater(const DriverManager &driver_manager, const NetworkManager &network_manager,
                 const MultiResolver &resolver_pool)
    : impl_(std::make_unique<Impl>(driver_manager, network_manager, resolver_pool)) {
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

    // --- Step 0: resolve driver ------------------------------------------------
    // The driver is guaranteed to exist: ConfigValidator already verified that every
    // domain's driver was loaded before the scheduler started.

    auto &driver = driver_manager_.get_driver(task.driver_name);

    // --- Step 1: local IP ---------------------------------------------------

    const auto local_ip = get_ip_address(task.subdomain);
    if (!local_ip) {
        SPDLOG_WARN("No valid IP address found for {}, skipping the update", task.fqdn);
        return;
    }

    // --- Step 2: skip if unchanged (unless force_update) --------------------

    if (!task.force_update) {
        const auto record = dns_lookup(task.fqdn, task.subdomain.type);

        if (record.has_value()) {
            if (record.value() == local_ip->to_string()) {
                SPDLOG_DEBUG("Domain: {}, type: {}, current {}, new {}, skipping update",
                             task.fqdn, rd_type, record.value(), local_ip->to_string());
                return;
            }

            SPDLOG_INFO(R"(Update needed, local IP "{}" != DNS record "{}")", local_ip->to_string(), record.value());
        }
    } else {
        SPDLOG_INFO("Force update triggered for {}", task.fqdn);
    }

    // --- Step 4: build parameters & generate request ------------------------

    const auto parameters = build_driver_parameters(task);
    const auto ctx = build_update_context(task, *local_ip, rd_type);

    // --- Step 5: delegate to driver via HttpClient --------------------------

    TransientHttpClient http_client{};
    if (!driver.execute(parameters, ctx, http_client)) {
        return;
    }

    SPDLOG_INFO("Update {}, type: {}, to {}", task.fqdn, rd_type, local_ip->to_string());
}

// ---------------------------------------------------------------------------
// dns_lookup — resolve a DNS name with retries.
// ---------------------------------------------------------------------------
std::optional<std::string>
Updater::Impl::dns_lookup(const std::string &host, dns_type type) const {
    return resolver_pool_.resolve(host, type);
}

// ---------------------------------------------------------------------------
// get_ip_address — obtain the local IP from an interface or a URL.
// ---------------------------------------------------------------------------
std::optional<InetAddress> Updater::Impl::get_ip_address(const Config::subdomain_config &config) const {
    auto address_family = DNS::dns2ip(config.type);
    if (config.ip_source == Config::ip_source_type::INTERFACE) {
        auto addresses = network_manager_.get_interface_ip_addresses(*config.interface);

        // Filter by address family.
        if (address_family != address_family_type::UNSPECIFIED) {
            std::erase_if(addresses, [af = address_family](const InetAddress &addr) {
                return addr.get_family() != af;
            });
        }

        if (address_family == address_family_type::IPV6) {
            // Filter out link-local addresses unless explicitly allowed.
            if (!config.allow_local_link) {
                std::erase_if(addresses, [](const InetAddress &addr) { return addr.is_link_local(); });
            }

            // Filter out unique-local addresses unless explicitly allowed.
            if (!config.allow_ula) {
                std::erase_if(addresses, [](const InetAddress &addr) { return addr.is_ula(); });
            }
        }

        if (!addresses.empty()) {
            return std::move(addresses.front());
        }
        return std::nullopt;
    }

    auto body = TransientHttpClient::get_body(config.ip_source_param, {
                                                  .address_family = address_family, .interface = config.interface
                                              });
    if (!body) {
        return std::nullopt;
    }

    StringUtil::trim(*body);
    SPDLOG_DEBUG("HTTP response: {}", *body);
    return InetAddress::parse(*body);
}

// ---------------------------------------------------------------------------
// build_driver_parameters — serialize subdomain.driver_param to JSON string.
// ---------------------------------------------------------------------------
driver_config_type Updater::Impl::build_driver_parameters(const UpdateTask &task) {
    return task.subdomain.driver_param.dump().value_or("{}");
}

// ---------------------------------------------------------------------------
// build_driver_context — assemble the runtime UpdateContext from the task and
//                        the resolved IP / record type.
// ---------------------------------------------------------------------------
UpdateContext
Updater::Impl::build_update_context(const UpdateTask &task, const InetAddress &ip_addr, std::string_view rd_type) {
    return {
        .ip_addr = ip_addr.to_string(),
        .rd_type = std::string(rd_type),
        .domain = task.domain_name,
        .subdomain = task.subdomain.name,
        .fqdn = task.fqdn,
    };
}
