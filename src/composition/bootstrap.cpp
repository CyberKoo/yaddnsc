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

#include "application/coro/run_scheduler.h"
#include "application/coro/services.h"
#include "application/diagnostics.h"
#include "application/environment_validator.h"
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
#include "infrastructure/dns/coro/factory.h"
#include "infrastructure/dns/coro/resolver_port.h"
#include "infrastructure/dns/factory.h"
#include "infrastructure/dns/resolv_conf.h"
#include "infrastructure/dns/resolver_catalog.h"
#include "infrastructure/ip_source/coro/adapter.h"
#include "infrastructure/logging/spdlog_logger.h"
#include "infrastructure/network/http/types.h"
#include "infrastructure/network/http/client.h"
#include "infrastructure/network/http/client_port.h"
#include "infrastructure/network/system_network_interfaces.h"
#include "infrastructure/network/uri.h"
#include "infrastructure/plugin/abi_driver_gateway.h"
#include "infrastructure/plugin/coro/driver_gateway.h"
#include "infrastructure/plugin/driver_catalog.h"
#include "infrastructure/plugin/driver_loader.h"
#include "support/exception.h"
#include "support/fmt.hpp"
#include "support/util/cancellation_token.hpp"

#include "version.h"

namespace {
/// Fill in the effective bootstrap DNS server list: the configured
/// bootstrap_dns wins; otherwise fall back to /etc/resolv.conf nameservers.
/// An empty result is not fatal — IP-literal targets still work — but every
/// hostname target (DoH/DoT server, HTTP IP source, provider API endpoint)
/// will fail fast at connect time, so say so once at startup.
void fill_bootstrap_servers(domain::RuntimeConfig& config) {
    if (!config.resolver.bootstrap_servers.empty()) {
        return;
    }
    config.resolver.bootstrap_servers = DNS::parse_resolv_conf();
    if (config.resolver.bootstrap_servers.empty()) {
        SPDLOG_WARN(
            "No bootstrap DNS servers available (no \"bootstrap_dns\" configured and no nameserver found in "
            "/etc/resolv.conf): hostname targets will fail to resolve. IP-literal targets are unaffected. "
            "(/etc/hosts and NSS are never consulted.)");
    }
}

/// Shared HTTP policy: user agent, CA discovery and bootstrap DNS are built
/// once here — every HTTP consumer (driver gateway, HTTP IP source) derives
/// its client options from this single source of truth.
[[nodiscard]] net::http::Options make_http_options(const domain::ResolverSettings& resolver) {
    net::http::Options opts;
    opts.user_agent = YADDNSC::get_full_version();
    opts.transport.bootstrap_dns = resolver.bootstrap_servers;
    return opts;
}

/// HTTP client factory for the driver gateway: the token is no longer bound
/// into the client — cancellation flows through each exchange() call instead.
[[nodiscard]] HttpClientFactory make_http_client_factory(net::http::Options opts) {
    return [opts = std::move(opts)] { return std::make_unique<net::http::Client>(opts); };
}

/// Shared coroutine HTTP policy for the run path (driver gateway + HTTP IP
/// source). The coroutine client has its own Options type; it carries the same
/// information as the legacy one (user agent, bootstrap DNS), plus the TLS and
/// connect policy defaults.
[[nodiscard]] http::Options make_coro_http_options(const domain::ResolverSettings& resolver) {
    http::Options opts;
    opts.user_agent = YADDNSC::get_full_version();
    opts.bootstrap_dns = resolver.bootstrap_servers;
    return opts;
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

int run_command(const Cli::RunCommand& command) {
    if (command.verbose) {
        spdlog::set_level(spdlog::level::debug);
        SPDLOG_DEBUG("Verbose mode enabled");
    }

    const auto raw_config = Config::load_config(command.config_path);

    // Static validation + normalisation: report every collected error with
    // the same output shape as the legacy ConfigVerificationException path.
    auto config = Config::validate_and_normalize(raw_config);
    if (!config.has_value()) {
        SPDLOG_CRITICAL(format_config_errors(config.error()));
        return EXIT_FAILURE;
    }
    fill_bootstrap_servers(*config);

    const auto runtime_config = std::make_shared<const domain::RuntimeConfig>(std::move(*config));

    // Configuration, driver loading and environment validation stay synchronous
    // ahead of the loop: the composition root owns the DriverCatalog and the
    // gateway it feeds, and folding dlopen into the loop would push those
    // infrastructure types into the application layer. Moving them behind
    // offload is stage-3 work; the run itself is the coroutine scheduler.
    {
        DriverCatalog driver_catalog;
        DriverLoader::load(driver_catalog, runtime_config->drivers);

        const SpdlogLogger logger;
        const SystemNetworkInterfaces interfaces;

        // Environment validation: referenced drivers loaded, referenced
        // interfaces present (all collected errors are reported).
        if (const auto env = validate_environment(*runtime_config, driver_catalog, interfaces); !env.has_value()) {
            SPDLOG_CRITICAL(format_config_errors(env.error()));
            return EXIT_FAILURE;
        }

        const auto http_options = make_coro_http_options(runtime_config->resolver);
        const auto dispatcher =
            dns::make_dispatcher(runtime_config->resolver, runtime_config->resolver.bootstrap_servers);
        dns::DispatcherResolverPort resolver_port{*dispatcher};
        ipsource::IpSourceAdapter ip_source{http_options};

        // The driver gateway needs the runner's bridge TaskGroup, which only
        // exists inside coro::run; the runner hands its root group to
        // make_gateway and the gateway it builds lives here for the whole run.
        std::optional<plugin::DriverGateway> gateway;
        coro::Loop loop;
        const app::RuntimeServices services{
            .resolver = resolver_port,
            .ip_source = ip_source,
            .logger = logger,
            .make_gateway =
                [&](coro::TaskGroup& group) -> app::GatewayPort& {
                gateway.emplace(driver_catalog, logger, loop, group,
                                plugin::DriverGateway::Options{
                                    .http = http_options,
                                    .bridge_wait_budget = std::chrono::seconds(5),
                                });
                return *gateway;
            },
        };
        return coro::run(loop, app::run_scheduler(runtime_config, services));
    }
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
    return Cli::present_driver_list(Diagnostics::list_drivers(catalog));
}

int execute_command(const Cli::DriverInfoCommand& command) {
    const auto catalog = load_catalog_for(command.config_path);
    return Cli::present_driver_info(catalog.describe(command.name));
}

int execute_command(const Cli::InterfaceListCommand&) {
    const SystemNetworkInterfaces interfaces;
    return Cli::present_interface_list(Diagnostics::list_interfaces(interfaces));
}

int execute_command(const Cli::InterfaceIpCommand& command) {
    const SystemNetworkInterfaces interfaces;
    return Cli::present_interface_ip(command.name, interfaces.addresses(command.name));
}

int execute_command(const Cli::DnsResolveCommand& command) {
    const auto config = load_runtime_config(command.config_path);
    auto dispatcher =
        DnsResolverFactory::create(config.resolver, ResolverCatalog::with_builtins(config.resolver.bootstrap_servers));
    // One-shot command: no SignalWatcher is installed, so Ctrl-C keeps the
    // default disposition and the resolve's cancellation token is
    // deliberately inert — the root owns no fd and adds nothing to poll
    // sets. (The dispatcher's per-batch race source still allocates its own
    // pipe, as it must: the winner cancels the losers through it.)
    return Cli::present_dns_resolve(
        Diagnostics::dns_resolve(dispatcher, command.host, command.type, Utils::CancellationToken{}));
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
    using Error = Diagnostics::ConfigTestError;

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
        if (const auto env = validate_environment(*config, driver_catalog, interfaces); !env.has_value()) {
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
        const SpdlogLogger logger;
        const AbiDriverGateway driver_gateway(driver_catalog,
                                              make_http_client_factory(make_http_options(config->resolver)), logger);
        for (const auto& domain_config : config->domains) {
            for (const auto& subdomain : domain_config.subdomains) {
                if (const auto result = driver_gateway.validate_config(domain_config.driver, subdomain.driver_params);
                    !result.has_value()) {
                    return Cli::present_config_test(
                        {.quiet = command.quiet,
                         .error = Error{.kind = Error::Kind::VERIFICATION,
                                        .message = fmt::format("Driver '{}' rejected configuration for {}: {}",
                                                               domain_config.driver,
                                                               domain::make_fqdn(domain_config.name, subdomain.name),
                                                               result.error().message)}});
                }
            }
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
