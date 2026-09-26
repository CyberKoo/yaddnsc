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
#include <string>
#include <string_view>
#include <thread>
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
#include "application/pool_task_executor.h"
#include "application/run_lifecycle.h"
#include "application/update_workflow.h"
#include "cli/presenter.h"
#include "domain/config/dns_config.h"
#include "domain/config/runtime_config.h"
#include "domain/error/error.h"
#include "domain/fqdn.h"
#include "infrastructure/config/config.h"
#include "infrastructure/config/config_verification_exception.h"
#include "infrastructure/config/normalizer.h"
#include "infrastructure/config/static_validator.h"
#include "infrastructure/dns/dispatcher.h"
#include "infrastructure/dns/factory.h"
#include "infrastructure/dns/resolv_conf.h"
#include "infrastructure/dns/resolver_catalog.h"
#include "infrastructure/ip_source/adapter.h"
#include "infrastructure/ip_source/factory.h"
#include "infrastructure/logging/spdlog_logger.h"
#include "infrastructure/network/http/client.h"
#include "infrastructure/network/http/client_port.h"
#include "infrastructure/network/http/types.h"
#include "infrastructure/network/system_network_interfaces.h"
#include "infrastructure/network/uri.h"
#include "infrastructure/plugin/abi_driver_gateway.h"
#include "infrastructure/plugin/driver_catalog.h"
#include "infrastructure/plugin/driver_loader.h"
#include "infrastructure/process/signal_watcher.h"
#include "infrastructure/time/steady_clock.h"
#include "support/exception.h"
#include "support/fmt.hpp"
#include "support/util/cancellation_token.hpp"

#include "version.h"

namespace {
/// Thread-pool sizing policy: total subdomains, capped at
/// min(hardware cores, 4); at least 2.
template<uint32_t THREAD_LIMIT = 4U>
std::uint32_t estimate_pool_size(const domain::RuntimeConfig& config) noexcept {
    std::uint32_t total_subdomains = 0;
    const auto thread_count = std::thread::hardware_concurrency();

    for (const auto& domain_config : config.domains) {
        total_subdomains += static_cast<std::uint32_t>(domain_config.subdomains.size());
    }

    if (total_subdomains < 2 || thread_count < 2) {
        return 2;
    }

    if (total_subdomains < thread_count) {
        return total_subdomains;
    }

    return std::min(thread_count, THREAD_LIMIT);
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
    config.resolver.bootstrap_servers = DNS::parse_resolv_conf();
    if (config.resolver.bootstrap_servers.empty()) {
        SPDLOG_WARN("No bootstrap DNS servers available (no \"bootstrap_dns\" configured and no nameserver found in "
                    "/etc/resolv.conf): hostname targets will fail to resolve. IP-literal targets are unaffected. "
                    "(/etc/hosts and NSS are never consulted.)");
    }
}

/// HTTP client factory for the driver gateway: the token is no longer bound
/// into the client — cancellation flows through each exchange() call instead.
[[nodiscard]] HttpClientFactory make_http_client_factory(std::vector<Config::DnsServer> bootstrap) {
    return [bootstrap = std::move(bootstrap)] {
        net::http::Options opts;
        opts.user_agent = YADDNSC::get_full_version();
        opts.transport.bootstrap_dns = bootstrap;
        return std::make_unique<net::http::Client>(std::move(opts));
    };
}

// -----------------------------------------------------------------------
//  run — the only command with a lifecycle object; exceptions escape to
//  main()'s fatal-error boundary (legacy wording preserved there).
// -----------------------------------------------------------------------
int run_command(const Cli::RunCommand& command) {
    if (command.verbose) {
        spdlog::set_level(spdlog::level::debug);
        SPDLOG_DEBUG("Verbose mode enabled");
    }

    SignalWatcher::install();

    const auto raw_config = Config::load_config(command.config_path);

    // Static validation + normalisation: report the first error with the
    // same output shape as the legacy ConfigVerificationException path.
    auto config = Config::validate_and_normalize(raw_config);
    if (!config.has_value()) {
        SPDLOG_CRITICAL(config.error().front().message);
        return EXIT_FAILURE;
    }
    fill_bootstrap_servers(*config);

    SignalWatcher signal_watcher;
    Utils::CancellationSource cancellation;
    const auto runtime_config = std::make_shared<const domain::RuntimeConfig>(std::move(*config));

    // The driver catalog lives in this scope: it is released (modules
    // unloaded) only after RunLifecycle::run() has drained every task,
    // so no in-flight update can touch unloaded code.
    {
        DriverCatalog driver_catalog;
        DriverLoader::load(driver_catalog, runtime_config->driver);

        const SpdlogLogger logger;
        SteadyClock clock;
        const SystemNetworkInterfaces interfaces;

        // Environment validation: referenced drivers loaded, referenced
        // interfaces present (same first-error wording as legacy).
        if (const auto env = validate_environment(*runtime_config, driver_catalog, interfaces); !env.has_value()) {
            SPDLOG_CRITICAL(env.error().front().message);
            return EXIT_FAILURE;
        }

        auto dispatcher = DnsResolverFactory::create(
            runtime_config->resolver, ResolverCatalog::with_builtins(runtime_config->resolver.bootstrap_servers));
        const auto& bootstrap = runtime_config->resolver.bootstrap_servers;
        const IpSourceAdapter ip_source{[&bootstrap](const domain::SubdomainConfig& cfg) {
            return IpSourceFactory::create(cfg, bootstrap);
        }};
        const AbiDriverGateway driver_gateway(driver_catalog, make_http_client_factory(bootstrap), logger);
        const UpdateWorkflow workflow(dispatcher, ip_source, driver_gateway, logger);
        PoolTaskExecutor task_executor(estimate_pool_size(*runtime_config), workflow);

        RunLifecycle lifecycle(runtime_config, signal_watcher.get_stop_source(), cancellation, clock, task_executor,
                               interfaces, logger);
        lifecycle.run();
    }
    return EXIT_SUCCESS;
}

[[nodiscard]] std::string format_resolver_server(const Config::DnsServer& server) {
    const auto uri = Uri::parse(server.address);
    if (!uri.get_schema().empty()) {
        std::string display = uri.get_origin();
        const auto path = uri.get_path();
        if (!path.empty() && path != "/") {
            display += path;
        }
        return display;
    }
    return fmt::format("{}:{}", uri.get_host_literal(), server.port);
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
        throw ConfigVerificationException(config.error().front().message);
    }
    fill_bootstrap_servers(*config);
    return std::move(*config);
}

/// Load the validated runtime configuration and fill a catalog for driver
/// diagnostics.
DriverCatalog load_catalog_for(const std::string& config_path) {
    const auto config = load_runtime_config(config_path);
    DriverCatalog catalog;
    DriverLoader::load(catalog, config.driver);
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
    // The one-shot command scope owns its own root cancellation source.
    Utils::CancellationSource cancellation;
    auto dispatcher =
        DnsResolverFactory::create(config.resolver, ResolverCatalog::with_builtins(config.resolver.bootstrap_servers));
    return Cli::present_dns_resolve(
        Diagnostics::dns_resolve(dispatcher, command.host, command.type, cancellation.token()));
}

int execute_command(const Cli::DnsResolverCommand& command) {
    const auto resolver = Config::load_config(command.config_path).resolver;
    std::vector<std::string> servers;
    servers.reserve(resolver.servers.size());
    for (const auto& server : resolver.servers) {
        servers.push_back(format_resolver_server(server));
    }
    return Cli::present_dns_resolver(resolver.use_custom_server, magic_enum::enum_name(resolver.strategy), servers,
                                     resolver.address, resolver.port);
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

        // Static checks first (collected as values; report the first one
        // with the same output shape as the legacy exception path).
        auto config = Config::validate_and_normalize(raw_config);
        if (!config.has_value()) {
            return Cli::present_config_test(
                {.quiet = command.quiet,
                 .error = Error{.kind = Error::Kind::VERIFICATION, .message = config.error().front().message}});
        }
        fill_bootstrap_servers(*config);

        DriverCatalog driver_catalog;
        DriverLoader::load(driver_catalog, config->driver);
        const SystemNetworkInterfaces interfaces;
        if (const auto env = validate_environment(*config, driver_catalog, interfaces); !env.has_value()) {
            return Cli::present_config_test(
                {.quiet = command.quiet,
                 .error = Error{.kind = Error::Kind::VERIFICATION, .message = env.error().front().message}});
        }

        // Driver-side driver_param validation through the OPTIONAL ABI
        // entry: every subdomain's driver_param is checked against its
        // driver's schema so a missing zone_id-style key fails here
        // instead of on the first update. Plugins that do not export
        // yaddnsc_driver_validate are skipped (not an error).
        Utils::CancellationSource cancellation;
        const SpdlogLogger logger;
        const AbiDriverGateway driver_gateway(driver_catalog, make_http_client_factory(config->resolver.bootstrap_servers),
                                              logger);
        for (const auto& domain_config : config->domains) {
            for (const auto& subdomain : domain_config.subdomains) {
                if (const auto result = driver_gateway.validate_config(domain_config.driver, subdomain.driver_param);
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
