//
// Created by Kotarou on 2026/9/17.
//

#include "bootstrap.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <expected>
#include <magic_enum/magic_enum.hpp>
#include <spdlog/spdlog.h>
#include <yaddnsc/util/format.hpp>

#include "application/diagnostics.h"
#include "application/environment_validator.h"
#include "application/run_scheduler.h"
#include "application/services.h"
#include "cli/presenter.h"
#include "domain/config/dns_config.h"
#include "domain/config/runtime_config.h"
#include "domain/error/error.h"
#include "domain/fqdn.h"
#include "infrastructure/config/config.h"
#include "infrastructure/config/config_verification_exception.h"
#include "infrastructure/config/normalizer.h"
#include "infrastructure/config/static_validator.h"
#include "infrastructure/coro/group.hpp"
#include "infrastructure/coro/loop.h"
#include "infrastructure/coro/run.hpp"
#include "infrastructure/coro/scope.hpp"
#include "infrastructure/dns/factory.h"
#include "infrastructure/dns/resolver_port.h"
#include "infrastructure/dns/resolv_conf.h"
#include "infrastructure/ip_source/adapter.h"
#include "infrastructure/ip_source/system_network_interfaces.h"
#include "infrastructure/logging/async_logging.h"
#include "infrastructure/logging/spdlog_logger.h"
#include "infrastructure/net/http/types.h"
#include "infrastructure/net/http/uri.h"
#include "infrastructure/net/tls_context.h"
#include "infrastructure/plugin/driver_catalog.h"
#include "infrastructure/plugin/driver_gateway.h"
#include "infrastructure/plugin/driver_loader.h"
#include "support/exception.h"
#include "support/fmt.hpp"

#include "version.h"

namespace {
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

/// Shared coroutine HTTP policy for the run path (driver gateway + HTTP IP
/// source). The coroutine client has its own Options type; it carries the same
/// information as the legacy one (user agent, bootstrap DNS), plus the TLS and
/// connect policy defaults.
[[nodiscard]] http::Options make_coro_http_options(const domain::ResolverSettings& resolver,
                                                   std::shared_ptr<const net::TlsContext> tls_context) {
    http::Options opts;
    opts.user_agent = YADDNSC::get_full_version();
    opts.bootstrap_dns = resolver.bootstrap_servers;
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

// -----------------------------------------------------------------------
//  run — the only command with a lifecycle object; exceptions escape to
//  main()'s fatal-error boundary (legacy wording preserved there).
// -----------------------------------------------------------------------

/// Join every collected validation error into one message so a single
/// failing run reports all problems instead of only the first.
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

/// Startup preparation and the run root.
///
/// Startup is synchronous on the main thread, before the loop exists: reading and
/// validating the configuration, dlopen-ing plugins, checking the environment and
/// building the trust context are all blocking file/loader I/O that has no reason
/// to touch the loop. Only the scheduler then runs inside coro::run.
///
/// Failure: a malformed/missing config or a plugin-load failure throws out of
/// here to main's fatal boundary with the legacy wording; a statically invalid
/// config or a failed environment check is logged and returns EXIT_FAILURE,
/// exactly as before.
int run_command(const Cli::RunCommand& command) {
    if (!command.log_level.empty()) {
        spdlog::set_level(to_log_level(command.log_level));
        SPDLOG_DEBUG("Log level set to {}", command.log_level);
    } else if (command.verbose) {
        spdlog::set_level(spdlog::level::debug);
        SPDLOG_DEBUG("Verbose mode enabled");
    }

    auto loaded = Config::validate_and_normalize(Config::load_config(command.config_path));
    if (!loaded.has_value()) {
        SPDLOG_CRITICAL(format_config_errors(loaded.error()));
        return EXIT_FAILURE;
    }
    fill_bootstrap_servers(*loaded);
    const auto runtime = std::make_shared<const domain::RuntimeConfig>(std::move(*loaded));

    DriverCatalog catalog;
    DriverLoader::load(catalog, runtime->drivers);

    const SpdlogLogger logger;
    const SystemNetworkInterfaces interfaces;
    if (const auto env = app::validate_environment(*runtime, catalog, interfaces); !env.has_value()) {
        SPDLOG_CRITICAL(format_config_errors(env.error()));
        return EXIT_FAILURE;
    }
    SPDLOG_INFO("All available interfaces: {}", fmt::format("{}", fmt::join(interfaces.names(), ", ")));

    const auto tls_context = make_default_tls_context();
    const auto http_options = make_coro_http_options(runtime->resolver, tls_context);
    const auto dispatcher =
        dns::make_dispatcher(runtime->resolver, runtime->resolver.bootstrap_servers, tls_context);
    dns::DispatcherResolverPort resolver_port{*dispatcher};
    ipsource::IpSourceAdapter ip_source{http_options};

    // The loop object is created here, but nothing runs until coro::run below.
    coro::Loop loop;

    // The driver gateway needs the runner's bridge TaskGroup, which only exists
    // inside the scheduler; the runner hands its root group to make_gateway and
    // the gateway it builds lives in this frame for the whole run.
    std::optional<plugin::DriverGateway> gateway;
    const app::RuntimeServices services{
        .resolver = resolver_port,
        .ip_source = ip_source,
        .logger = logger,
        .make_gateway =
            [&](coro::TaskGroup& group) -> app::GatewayPort& {
            gateway.emplace(catalog, logger, loop, group,
                            plugin::DriverGateway::Options{
                                .http = http_options,
                                .bridge_wait_budget = std::chrono::seconds(5),
                            });
            return *gateway;
        },
        .drain_logs = &logging::shutdown,
    };
    return coro::run(loop, app::run_scheduler(runtime, services));
}

[[nodiscard]] std::string format_resolver_server(const Config::DnsServer& server) {
    const auto uri = Uri::parse(server.address);
    if (!uri.has_value()) {
        // Display helper must never fail: show the raw address as-is.
        return server.address;
    }
    if (!uri->get_schema().empty()) {
        std::string display = uri->get_origin();
        const auto path = uri->get_path();
        if (!path.empty() && path != "/") {
            display += path;
        }
        return display;
    }
    return fmt::format("{}:{}", uri->get_host_literal(), server.port);
}

// -----------------------------------------------------------------------
//  Diagnostic commands — composition root calls the ports directly and
//  hands the result objects to the presenter.
// -----------------------------------------------------------------------

/// Load exactly one validated runtime configuration for a composition path.
/// No runtime dependency may re-normalize the raw DTO afterwards.
[[nodiscard]] domain::RuntimeConfig load_runtime_config(const std::string& config_path) {
    auto config = Config::validate_and_normalize(Config::load_config(config_path));
    if (!config.has_value()) {
        throw ConfigVerificationException(format_config_errors(config.error()));
    }
    fill_bootstrap_servers(*config);
    return std::move(*config);
}

/// Load the validated runtime configuration and fill a catalog for driver
/// diagnostics.
DriverCatalog load_catalog_for(const std::string& config_path) {
    const auto config = load_runtime_config(config_path);
    DriverCatalog catalog;
    DriverLoader::load(catalog, config.drivers);
    return catalog;
}

int execute_command(const Cli::DriverListCommand& command) {
    const auto catalog = load_catalog_for(command.config_path);
    return Cli::present_driver_list(app::list_drivers(catalog));
}

int execute_command(const Cli::DriverInfoCommand& command) {
    const auto catalog = load_catalog_for(command.config_path);
    return Cli::present_driver_info(catalog.describe(command.name));
}

int execute_command(const Cli::InterfaceListCommand&) {
    const SystemNetworkInterfaces interfaces;
    return Cli::present_interface_list(app::list_interfaces(interfaces));
}

int execute_command(const Cli::InterfaceIpCommand& command) {
    const SystemNetworkInterfaces interfaces;
    return Cli::present_interface_ip(command.name, interfaces.addresses(command.name));
}

int execute_command(const Cli::DnsResolveCommand& command) {
    const auto config = load_runtime_config(command.config_path);
    const auto tls_context = make_default_tls_context();
    const auto dispatcher = dns::make_dispatcher(config.resolver, config.resolver.bootstrap_servers, tls_context);
    dns::DispatcherResolverPort resolver_port{*dispatcher};
    // One-shot command on a plain root scope: nothing is marked cancellable, so
    // Ctrl-C keeps the default disposition (no signal watcher is installed).
    // The resolvers bound each operation, but the command still gets an overall
    // budget so a pathological attempt chain always terminates; 30s covers the
    // legacy worst case (bootstrap + two DoT/DoH attempts of connect + I/O).
    constexpr auto DNS_COMMAND_BUDGET = std::chrono::seconds{30};
    coro::Loop loop;
    const app::DnsResolveOutcome outcome =
        coro::run(loop, [&]() -> coro::Task<app::DnsResolveOutcome> {
            auto bounded = co_await coro::with_timeout(
                DNS_COMMAND_BUDGET, [&](coro::CancelScope&) -> coro::Task<app::DnsResolveOutcome> {
                    co_return co_await app::dns_resolve(resolver_port, command.host, command.type);
                });
            if (bounded.timed_out) {
                // Own budget fired (checked before `cancelled`): report it like
                // a resolver failure — the presenter prints it and exits 0, the
                // same shape a bounded lookup produced on the legacy stack.
                co_return app::DnsResolveOutcome{
                    .host = command.host,
                    .type_text = command.type,
                    .lookup = std::unexpected(DnsErrorInfo{
                        DnsError::CONNECTION,
                        fmt::format("DNS lookup timed out after {}s",
                                    std::chrono::duration_cast<std::chrono::seconds>(DNS_COMMAND_BUDGET).count())}),
                };
            }
            co_return std::move(*bounded);
        }());
    return Cli::present_dns_resolve(outcome);
}

int execute_command(const Cli::DnsResolverCommand& command) {
    const auto resolver = Config::load_config(command.config_path).resolver;
    std::vector<std::string> servers;
    servers.reserve(resolver.servers.size());
    for (const auto& server : resolver.servers) {
        servers.push_back(format_resolver_server(server));
    }
    return Cli::present_dns_resolver(resolver.use_custom_servers, magic_enum::enum_name(resolver.strategy), servers);
}

int execute_command(const Cli::ConfigShowCommand& command) {
    return Cli::present_config_show(Config::redacted_json(Config::load_config(command.config_path)));
}

int execute_command(const Cli::ConfigTestCommand& command) {
    using Error = app::ConfigTestError;

    if (command.quiet) {
        spdlog::set_level(spdlog::level::off);
    }

    try {
        const auto raw_config = Config::load_config(command.config_path);

        // Static checks first (collected as values; every error is reported
        // with the same output shape as the legacy exception path).
        auto config = Config::validate_and_normalize(raw_config);
        if (!config.has_value()) {
            return Cli::present_config_test(
                {.quiet = command.quiet,
                 .error = Error{.kind = Error::Kind::VERIFICATION, .message = format_config_errors(config.error())}});
        }
        fill_bootstrap_servers(*config);

        DriverCatalog driver_catalog;
        DriverLoader::load(driver_catalog, config->drivers);
        const SystemNetworkInterfaces interfaces;
        if (const auto env = app::validate_environment(*config, driver_catalog, interfaces); !env.has_value()) {
            return Cli::present_config_test(
                {.quiet = command.quiet,
                 .error = Error{.kind = Error::Kind::VERIFICATION, .message = format_config_errors(env.error())}});
        }

        // Driver-side driver_params validation through the OPTIONAL ABI
        // entry: every subdomain's driver_params is checked against its
        // driver's schema so a missing zone_id-style key fails here
        // instead of on the first update. A plugin that does not export
        // yaddnsc_driver_validate fails this check: the host cannot confirm
        // the configuration. The plugin can still be loaded for updates.
        // The coroutine gateway needs a loop and a bridge group, so the check
        // runs in a one-shot loop whose group owns the bridge coroutines.
        const SpdlogLogger logger;
        // validate_config runs with make_services(false), so the plugin cannot
        // reach http_exchange here and no TLS context is needed.
        const auto http_options = make_coro_http_options(config->resolver, nullptr);
        coro::Loop loop;
        std::optional<std::string> rejected;
        coro::run(loop, [&]() -> coro::Task<void> {
            co_await coro::supervisor_group([&](coro::TaskGroup& group) -> coro::Task<void> {
                plugin::DriverGateway gateway(driver_catalog, logger, loop, group,
                                              plugin::DriverGateway::Options{
                                                  .http = http_options,
                                                  .bridge_wait_budget = std::chrono::seconds(5),
                                              });
                for (const auto& domain_config : config->domains) {
                    for (const auto& subdomain : domain_config.subdomains) {
                        const auto result =
                            co_await gateway.validate_config(domain_config.driver, subdomain.driver_params);
                        if (!result.has_value()) {
                            rejected = fmt::format("Driver '{}' rejected configuration for {}: {}",
                                                   domain_config.driver,
                                                   domain::make_fqdn(domain_config.name, subdomain.name),
                                                   result.error().message);
                            co_return;
                        }
                    }
                }
                co_return;
            });
            co_return;
        }());
        if (rejected.has_value()) {
            return Cli::present_config_test({.quiet = command.quiet,
                                             .error = Error{.kind = Error::Kind::VERIFICATION, .message = *rejected}});
        }

        return Cli::present_config_test({.quiet = command.quiet, .error = std::nullopt});
    } catch (const ConfigVerificationException& e) {
        return Cli::present_config_test(
            {.quiet = command.quiet, .error = Error{.kind = Error::Kind::VERIFICATION, .message = e.what()}});
    } catch (const YaddnscException& e) {
        return Cli::present_config_test(
            {.quiet = command.quiet, .error = Error{.kind = Error::Kind::FATAL, .message = e.what()}});
    } catch (const std::exception& e) {
        return Cli::present_config_test(
            {.quiet = command.quiet, .error = Error{.kind = Error::Kind::GENERIC, .message = e.what()}});
    }
}

int execute_command(const Cli::InfoCommand&) {
    return Cli::present_info();
}
}  // anonymous namespace

void Composition::initialize_logging() { logging::initialize(); }

void Composition::shutdown_logging() noexcept { logging::shutdown(); }

int Composition::dispatch(const Cli::Command& command) {
    // RUN keeps its own error boundary in main() (fatal log lines).
    if (const auto* run = std::get_if<Cli::RunCommand>(&command)) {
        return run_command(*run);
    }

    // Diagnostic commands: any escaping failure is presented as
    // "Error: <what>" — the legacy catch-all wording.
    try {
        return std::visit(
            []<typename CommandT>(const CommandT& cmd) {
                // The RunCommand alternative never reaches the visitor (it is
                // guarded above); the branch only completes the overload set.
                if constexpr (std::is_same_v<CommandT, Cli::RunCommand>) {
                    return run_command(cmd);
                } else {
                    return execute_command(cmd);
                }
            },
            command);
    } catch (const std::exception& e) {
        return Cli::present_error(e);
    }
}
