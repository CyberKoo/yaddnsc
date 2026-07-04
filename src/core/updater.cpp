//
// Created by Kotarou on 2026/6/18.
//

#include "updater.h"

#include <spdlog/spdlog.h>
#include <magic_enum/magic_enum.hpp>

#include "update_task.h"
#include "dns/dispatcher.h"
#include "driver_manager.h"
#include "interface/driver.h"
#include "interface/http_client.h"
#include "network/http_client.h"
#include "ip_source/base.h"
#include "ip_source/factory.h"

namespace {
    // Filter out link-local and ULA addresses for AAAA candidates.
    void filter_ipv6_candidates(std::vector<InetAddress> &candidates, const Config::SubdomainConfig &config) {
        if (!config.allow_local_link) {
            std::erase_if(candidates, [](const InetAddress &a) { return a.is_link_local(); });
        }
        if (!config.allow_ula) {
            std::erase_if(candidates, [](const InetAddress &a) { return a.is_ula(); });
        }
    }
} // anonymous namespace

// ===========================================================================
//  Updater::Impl  —  all private helpers live here.
// ===========================================================================

struct Updater::Impl {
    explicit Impl(const DriverManager &driver_manager, const ResolverDispatcher &resolver_dispatcher);

    void process(const UpdateTask &task) const;

    [[nodiscard]] std::vector<std::string> dns_lookup(const std::string &host, DNS::Type type) const;

    [[nodiscard]] static std::optional<InetAddress> resolve_local_address(const Config::SubdomainConfig &config);

    [[nodiscard]] static DriverConfig build_driver_parameters(const UpdateTask &task);

    [[nodiscard]] static UpdateContext
    build_update_context(const UpdateTask &task, const InetAddress &ip_addr, std::string_view rd_type);

    const DriverManager &driver_manager_;
    const ResolverDispatcher &dispatcher_;
};

Updater::Impl::Impl(const DriverManager &driver_manager, const ResolverDispatcher &resolver_dispatcher)
    : driver_manager_(driver_manager), dispatcher_(resolver_dispatcher) {
}

void Updater::Impl::process(const UpdateTask &task) const {
    auto rd_type_name = magic_enum::enum_name(task.config.type);
    const auto rd_type = rd_type_name.empty() ? "UNKNOWN" : rd_type_name;

    // --- Step 0: resolve driver ------------------------------------------------
    // The driver is guaranteed to exist: ConfigValidator already verified that every
    // domain's driver was loaded before the scheduler started.

    auto &driver = driver_manager_.get_driver(task.driver_name);

    // --- Step 1: local IP ---------------------------------------------------

    const auto local_ip = resolve_local_address(task.config);
    if (!local_ip) {
        SPDLOG_WARN("No valid IP address found for {}, skipping the update", task.fqdn);
        return;
    }

    // --- Step 2: skip if unchanged (unless force_update) --------------------

    if (!task.force_update) {
        const auto records = dns_lookup(task.fqdn, task.config.type);

        if (!records.empty()) {
            const auto &first = records.front();
            if (first == local_ip->to_string()) {
                SPDLOG_DEBUG("Domain: {}, type: {}, current {}, new {}, skipping update", task.fqdn, rd_type, first,
                             local_ip->to_string());
                return;
            }

            SPDLOG_INFO(R"(Update needed, local IP "{}" != DNS record "{}")", local_ip->to_string(), first);
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

std::vector<std::string>
Updater::Impl::dns_lookup(const std::string &host, DNS::Type type) const {
    return dispatcher_.resolve(host, type);
}

std::optional<InetAddress> Updater::Impl::resolve_local_address(const Config::SubdomainConfig &config) {
    auto ip_source = IpSourceFactory::create(config);
    auto candidates = ip_source->resolve();

    if (candidates.empty()) {
        return std::nullopt;
    }

    // Only AAAA records need link-local / ULA filtering.
    if (config.type == DNS::Type::AAAA) {
        filter_ipv6_candidates(candidates, config);

        if (candidates.empty()) {
            return std::nullopt;
        }
    }

    return candidates.front();
}

DriverConfig Updater::Impl::build_driver_parameters(const UpdateTask &task) {
    return task.config.driver_param.dump().value_or("{}");
}

UpdateContext
Updater::Impl::build_update_context(const UpdateTask &task, const InetAddress &ip_addr, std::string_view rd_type) {
    return {
        .ip_addr = ip_addr.to_string(),
        .rd_type = std::string(rd_type),
        .domain = task.domain_name,
        .subdomain = task.config.name,
        .fqdn = task.fqdn,
    };
}

// ===========================================================================
//  Updater public API — thin delegation to Impl
// ===========================================================================

Updater::Updater(const DriverManager &driver_manager, const ResolverDispatcher &resolver_pool)
    : impl_(std::make_unique<Impl>(driver_manager, resolver_pool)) {
}

Updater::~Updater() = default;

void Updater::process(const UpdateTask &task) const noexcept {
    try {
        impl_->process(task);
    } catch (const std::exception &e) {
        SPDLOG_ERROR("Unhandled exception during update for {}: {}", task.fqdn, e.what());
    } catch (...) {
        SPDLOG_ERROR("Unknown non-standard exception during update for {}", task.fqdn);
    }
}
