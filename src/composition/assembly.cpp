#include "assembly.h"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <expected>
#include <spdlog/spdlog.h>

#include "coro/loop.h"
#include "domain/config/dns_config.h"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include "domain/error/error.h"
#include "infrastructure/config/config.h"
#include "infrastructure/config/config_exception.h"
#include "infrastructure/config/static_validator.h"
#include "infrastructure/dns/bootstrap/bootstrap.h"
#include "infrastructure/dns/bootstrap/resolv_conf.h"
#include "infrastructure/dns/dispatcher.h"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include "infrastructure/dns/factory.h"
#include "infrastructure/network/tls/context.h"
#include "infrastructure/network/transport/options.h"
#include "infrastructure/plugin/driver_loader.h"

#include "version.h"

namespace {
/// Loop trace diagnostics ride the same compile-time gate as the rest of the
/// trace logging (YADDNSC_ENABLE_TRACE): when traces are compiled out the sink
/// stays null and every loop trace site costs one null check. When compiled
/// in, the runtime level filter (--log-level) applies inside spdlog as usual.
/// Reached only through make_traced_loop, which is the single place a command
/// obtains a loop.
void install_loop_trace_sink(coro::Loop& loop) {
#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_TRACE
    loop.set_trace_sink(
        [](void*, std::string_view message, const std::source_location& where) noexcept {
            try {
                spdlog::default_logger_raw()->log(
                    spdlog::source_loc{where.file_name(), static_cast<int>(where.line()), where.function_name()},
                    spdlog::level::trace, "{}", message);
            } catch (...) {
                // A diagnostic callback must not propagate into the loop.
            }
        },
        nullptr);
#else
    static_cast<void>(loop);
#endif
}
}  // namespace

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
/// Options type; it carries the user agent and bootstrap DNS, plus the TLS and
/// connect policy defaults.
[[nodiscard]] http::Options make_coro_http_options(const domain::ResolverSettings& resolver,
                                                   std::shared_ptr<const net::TlsContext> tls_context) {
    http::Options opts;
    opts.user_agent = YADDNSC::get_full_version();
    opts.resolve = dns::make_bootstrap_resolver(resolver.bootstrap_servers, DNS_POLICY);
    opts.tls_context = std::move(tls_context);
    return opts;
}

/// Build the shared default trust context off the loop. CA discovery and load are
/// blocking file I/O, so this must run before `coro::run`. A machine without a
/// usable trust store still runs: the context comes back null and every TLS
/// connection then fails closed at connect, the same outcome the per-stream lazy
/// load produced.
[[nodiscard]] std::shared_ptr<const net::TlsContext> make_default_tls_context() {
    auto created = net::TlsContext::create(net::TlsTrustOptions{});
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

[[nodiscard]] std::unique_ptr<dns::Dispatcher> make_dispatcher(const domain::ResolverSettings& resolver,
                                                               const std::shared_ptr<const net::TlsContext>& tls_context) {
    try {
        return dns::make_dispatcher(resolver, resolver.bootstrap_servers, tls_context, DNS_POLICY);
    } catch (const std::invalid_argument& error) {
        throw ConfigException(error.what());
    }
}

[[nodiscard]] std::unique_ptr<coro::Loop> make_traced_loop() {
    auto loop = std::make_unique<coro::Loop>();
    install_loop_trace_sink(*loop);
    return loop;
}

[[nodiscard]] std::unique_ptr<plugin::DriverGateway> make_driver_gateway(const DriverCatalog& catalog,
                                                                         const app::LoggerPort& logger,
                                                                         coro::Loop& loop, coro::Nursery& bridge_group,
                                                                         const http::Options& http_options) {
    return std::make_unique<plugin::DriverGateway>(catalog, logger, loop, bridge_group,
                                                   make_gateway_options(http_options));
}

[[nodiscard]] int run_exit_code(app::MainLoopOutcome outcome) noexcept {
    return outcome == app::MainLoopOutcome::GracefulShutdown ? EXIT_SUCCESS : EXIT_FAILURE;
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
