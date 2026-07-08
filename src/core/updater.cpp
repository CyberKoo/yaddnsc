//
// Created by Kotarou on 2026/6/18.
//

#include "updater.h"

#include <spdlog/spdlog.h>
#include <magic_enum/magic_enum.hpp>

#include "update_task.h"
#include "dns/dispatcher.h"
#include "interface/driver.h"
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

    std::unique_ptr<IpSourceBase> default_ip_source_factory(const Config::SubdomainConfig &cfg) {
        return IpSourceFactory::create(cfg);
    }
} // anonymous namespace

// ===========================================================================
//  Updater::Impl  —  all private helpers live here.
// ===========================================================================

struct Updater::Impl {
    using IpSourceFactoryFunc = std::function<std::unique_ptr<IpSourceBase>(const Config::SubdomainConfig &)>;

    explicit Impl(const ResolverDispatcher &resolver_dispatcher, IpSourceFactoryFunc factory);

    /// Execute a single update task: resolve local IP, compare with DNS
    /// record, and invoke the driver if the IP has changed.
    void process(const UpdateTask &task, const Driver &driver, HttpClient &http_client) const;

    /// Perform a DNS lookup for the given host and record type.
    [[nodiscard]] std::vector<std::string> dns_lookup(const std::string &host, RecordKind type) const;

    /// Resolve the local IP address from the configured IP source.
    [[nodiscard]] std::optional<InetAddress> resolve_local_address(const Config::SubdomainConfig &config) const;

    /// Build the driver configuration string from the update task.
    [[nodiscard]] static DriverConfig build_driver_parameters(const UpdateTask &task);

    /// Build the per-update parameter struct for the driver.
    [[nodiscard]] static DriverUpdateParams
    build_update_context(const UpdateTask &task, const InetAddress &ip_addr, std::string_view rd_type);

    /// Non-owning reference to the resolver dispatcher (owned by Manager::Impl).
    const ResolverDispatcher &dispatcher_;

    /// IP source factory (injectable for testing).
    IpSourceFactoryFunc ip_factory_;
};

Updater::Impl::Impl(const ResolverDispatcher &resolver_dispatcher, IpSourceFactoryFunc factory)
    : dispatcher_(resolver_dispatcher), ip_factory_(std::move(factory)) {
}

void Updater::Impl::process(const UpdateTask &task, const Driver &driver, HttpClient &http_client) const {
    auto rd_type_name = magic_enum::enum_name(task.config.type);
    const auto rd_type = rd_type_name.empty() ? "UNKNOWN" : rd_type_name;

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
                SPDLOG_DEBUG("Domain {} ({}) unchanged ({}), skipping update", task.fqdn, rd_type, first);
                return;
            }

            SPDLOG_DEBUG("Domain {} ({}) will be updated to {} (was {})", task.fqdn, rd_type, local_ip->to_string(), first);
        }
    } else {
        SPDLOG_INFO("Force update triggered for {}", task.fqdn);
    }

    // --- Step 3: build parameters & generate request ------------------------

    const auto parameters = build_driver_parameters(task);
    const auto ctx = build_update_context(task, *local_ip, rd_type);

    // --- Step 4: delegate to driver via HttpClient --------------------------

    if (!driver.execute(parameters, ctx, http_client)) {
        return;
    }

    SPDLOG_INFO("Domain {} ({}) updated to {}", task.fqdn, rd_type, local_ip->to_string());
}

std::vector<std::string>
Updater::Impl::dns_lookup(const std::string &host, RecordKind type) const {
    return dispatcher_.resolve(host, type);
}

std::optional<InetAddress> Updater::Impl::resolve_local_address(const Config::SubdomainConfig &config) const {
    try {
        auto ip_source = ip_factory_(config);
        auto candidates = ip_source->resolve();

        if (candidates.empty()) {
            return std::nullopt;
        }

        // Only AAAA records need link-local / ULA filtering.
        if (config.type == RecordKind::AAAA) {
            filter_ipv6_candidates(candidates, config);

            if (candidates.empty()) {
                return std::nullopt;
            }
        }

        return candidates.front();
    } catch (const std::exception &e) {
        SPDLOG_WARN(R"(IP source resolution failed: {})", e.what());
        return std::nullopt;
    }
}

DriverConfig Updater::Impl::build_driver_parameters(const UpdateTask &task) {
    return task.config.driver_param.dump().value_or("{}");
}

DriverUpdateParams
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

Updater::Updater(const ResolverDispatcher &resolver_pool)
    : Updater(resolver_pool, default_ip_source_factory) {
}

Updater::Updater(const ResolverDispatcher &resolver_pool, IpSourceFactory ip_factory)
    : impl_(std::make_unique<Impl>(resolver_pool, std::move(ip_factory))) {
}

Updater::~Updater() = default;

void Updater::process(const UpdateTask &task, const Driver &driver, HttpClient &http_client) const noexcept {
    try {
        impl_->process(task, driver, http_client);
    } catch (const std::exception &e) {
        SPDLOG_ERROR("Unhandled exception during update for {}: {}", task.fqdn, e.what());
    } catch (...) {
        SPDLOG_ERROR("Unknown non-standard exception during update for {}", task.fqdn);
    }
}
