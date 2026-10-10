#include "assembly.h"

#include <spdlog/spdlog.h>
#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <expected>
#include <vector>

#include "domain/config/dns_config.h"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include "infrastructure/config/config.h"
#include "infrastructure/config/config_exception.h"
#include "infrastructure/config/static_validator.h"
#include "infrastructure/dns/bootstrap/bootstrap.h"
#include "infrastructure/dns/bootstrap/resolv_conf.h"
#include "infrastructure/network/tls/context.h"
#include "infrastructure/plugin/driver_loader.h"
#include "version.h"
#include "domain/error/error.h"
#include "infrastructure/network/transport/options.h"

namespace Composition::internal {
/// Map a --log-level token to the spdlog level. The CLI restricts the value
/// to these five tokens, so the fallthrough is unreachable.
[[nodiscard]] spdlog::level::level_enum to_log_level(std::string_view level) {
    if (level == "trace") {
        return spdlog::level::trace;
    }
    if (level == "debug") {
        return spdlog::level::debug;
    }
    if (level == "info") {
        return spdlog::level::info;
    }
    if (level == "warn") {
        return spdlog::level::warn;
    }
    return spdlog::level::err;
}

/// Fill in the effective bootstrap DNS server list: the configured
/// bootstrap_dns wins; otherwise fall back to /etc/resolv.conf nameservers.
/// An empty result is not fatal — IP-literal targets still work — but every
/// hostname target (DoH/DoT server, HTTP IP source, provider API endpoint)
/// will fail fast at connect time, so say so once at startup.
void fill_bootstrap_servers(domain::RuntimeConfig& config) {
    if (!config.resolver.bootstrap_servers.empty()) {
        return;
    }
    config.resolver.bootstrap_servers = dns::parse_resolv_conf();
    if (config.resolver.bootstrap_servers.empty()) {
        SPDLOG_WARN(
            "No bootstrap DNS servers available (no \"bootstrap_dns\" configured and no nameserver found in "
            "/etc/resolv.conf): hostname targets will fail to resolve. IP-literal targets are unaffected. "
            "(/etc/hosts and NSS are never consulted.)");
    }
}

/// Shared coroutine HTTP policy for the commands that build coroutine HTTP
/// clients (driver gateway, HTTP IP source). The coroutine client has its own
/// Options type; it carries the same information as the legacy one (user agent,
/// bootstrap DNS), plus the TLS and connect policy defaults.
[[nodiscard]] http::Options make_coro_http_options(const domain::ResolverSettings& resolver,
                                                   std::shared_ptr<const net::TlsContext> tls_context) {
    http::Options opts;
    opts.user_agent = YADDNSC::get_full_version();
    opts.resolve = dns::make_bootstrap_resolver(resolver.bootstrap_servers);
    opts.tls_context = std::move(tls_context);
    return opts;
}

/// Build the shared default trust context off the loop. CA discovery and load are
/// blocking file I/O, so this must run before `coro::run`. A machine without a
/// usable trust store still runs: the context comes back null and every TLS
/// connection then fails closed at connect, the same outcome the per-stream lazy
/// load produced.
[[nodiscard]] std::shared_ptr<const net::TlsContext> make_default_tls_context() {
    auto created = net::TlsContext::create(net::TlsOptions{});
    if (!created) {
        SPDLOG_WARN("No usable TLS trust context could be built; TLS targets will fail to connect");
        return nullptr;
    }
    return std::move(*created);
}

/// Shared driver-gateway options for the run path and config test's
/// validation loop.
[[nodiscard]] plugin::DriverGateway::Options make_gateway_options(const http::Options& http_options) {
    return {.http = http_options, .bridge_wait_budget = std::chrono::seconds(5)};
}

// -----------------------------------------------------------------------
//  Shared assembly — the single entry points every command builds on.
// -----------------------------------------------------------------------

/// Join every collected validation error into one message so one command
/// reports all problems instead of only the first.
[[nodiscard]] std::string format_config_errors(std::span<const domain::ConfigError> errors) {
    std::string joined;
    for (const auto& error : errors) {
        if (!joined.empty()) {
            joined += '\n';
        }
        joined += error.message;
    }
    return joined;
}

/// Load exactly one validated runtime configuration for a composition path.
/// No runtime dependency may re-normalize the raw DTO afterwards.
[[nodiscard]] domain::RuntimeConfig load_runtime_config(const std::string& config_path) {
    auto config = Config::validate_and_normalize(Config::load_config(config_path));
    if (!config.has_value()) {
        throw ConfigException(format_config_errors(config.error()));
    }
    fill_bootstrap_servers(*config);
    return std::move(*config);
}

/// Fill a catalog with the configured drivers.
DriverCatalog load_catalog(const domain::RuntimeConfig& config) {
    DriverCatalog catalog;
    DriverLoader::load(catalog, config.drivers);
    return catalog;
}

}  // namespace Composition::internal
