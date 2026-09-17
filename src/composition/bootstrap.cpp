//
// Created by Kotarou on 2026/9/17.
//

#include "bootstrap.h"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <thread>
#include <utility>

#include <spdlog/spdlog.h>

#include "application/diagnostics.h"
#include "application/environment_validator.h"
#include "application/pool_task_executor.h"
#include "application/run_lifecycle.h"
#include "application/update_workflow.h"

#include "cli/presenter.h"

#include "config/config.h"
#include "config/normalizer.h"
#include "config/static_validator.h"

#include "core/driver_loader.h"
#include "core/signal_watcher.h"
#include "core/spdlog_logger.h"
#include "core/steady_clock.h"

#include "dns/factory.h"
#include "dns/resolver_catalog.h"

#include "exception/base.h"
#include "exception/config_verification.h"

#include "http_client/client.h"
#include "infrastructure/plugin/abi_driver_gateway.h"
#include "infrastructure/plugin/driver_catalog.h"
#include "ip_source/adapter.h"
#include "network/system_network_interfaces.h"
#include "util/cancellation_token.hpp"
#include "version.h"

namespace {
    /// Thread-pool sizing policy: total subdomains, capped at
    /// min(hardware cores, 4); at least 2.
    std::uint32_t estimate_pool_size(const domain::RuntimeConfig &config) noexcept {
        std::uint32_t total_subdomains = 0;
        const auto thread_count = std::thread::hardware_concurrency();

        for (const auto &domain_config: config.domains) {
            total_subdomains += static_cast<std::uint32_t>(domain_config.subdomains.size());
        }

        if (total_subdomains < 2 || thread_count < 2) {
            return 2;
        }

        if (total_subdomains < thread_count) {
            return total_subdomains;
        }

        return std::min(thread_count, 4U);
    }

    /// HTTP client factory bound to the run's cancellation source: every
    /// client created from it is cancellable through the same token.
    [[nodiscard]] HttpClientFactory make_http_client_factory(const std::shared_ptr<Utils::CancellationSource> &src) {
        return [src] {
            net::http::Options opts;
            opts.user_agent = YADDNSC::get_full_version();
            return std::make_unique<net::http::Client>(std::move(opts), src->token());
        };
    }

    // -----------------------------------------------------------------------
    //  run — the only command with a lifecycle object; exceptions escape to
    //  main()'s fatal-error boundary (legacy wording preserved there).
    // -----------------------------------------------------------------------
    int run_command(const Cli::RunCommand &command) {
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

        SignalWatcher signal_watcher;
        const auto cancel_src = std::make_shared<Utils::CancellationSource>();
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
            if (const auto env = validate_environment(*runtime_config, driver_catalog, interfaces);
                !env.has_value()) {
                SPDLOG_CRITICAL(env.error().front().message);
                return EXIT_FAILURE;
            }

            auto dispatcher =
                DnsResolverFactory::create(runtime_config->resolver, cancel_src->token(), ResolverCatalog::with_builtins());
            const IpSourceAdapter ip_source(cancel_src->token());
            const AbiDriverGateway driver_gateway(driver_catalog, make_http_client_factory(cancel_src),
                                                  cancel_src->token(), logger);
            const UpdateWorkflow workflow(dispatcher, ip_source, driver_gateway, logger);
            PoolTaskExecutor task_executor(estimate_pool_size(*runtime_config), workflow);

            RunLifecycle lifecycle(runtime_config, signal_watcher.get_stop_source(), cancel_src, clock,
                                   task_executor, interfaces, logger);
            lifecycle.run();
        }
        return EXIT_SUCCESS;
    }

    // -----------------------------------------------------------------------
    //  Diagnostic commands — composition root calls the ports directly and
    //  hands the result objects to the presenter.
    // -----------------------------------------------------------------------

    /// Load the raw config and fill a catalog for the driver diagnostics:
    /// normalise only — these commands deliberately perform no validation.
    DriverCatalog load_catalog_for(const std::string &config_path) {
        DriverCatalog catalog;
        DriverLoader::load(catalog, Config::normalize(Config::load_config(config_path)).driver);
        return catalog;
    }

    int execute_command(const Cli::DriverListCommand &command) {
        const auto catalog = load_catalog_for(command.config_path);
        return Cli::present_driver_list(Diagnostics::list_drivers(catalog));
    }

    int execute_command(const Cli::DriverInfoCommand &command) {
        const auto catalog = load_catalog_for(command.config_path);
        return Cli::present_driver_info(catalog.describe(command.name));
    }

    int execute_command(const Cli::InterfaceListCommand &) {
        const SystemNetworkInterfaces interfaces;
        return Cli::present_interface_list(Diagnostics::list_interfaces(interfaces));
    }

    int execute_command(const Cli::InterfaceIpCommand &command) {
        const SystemNetworkInterfaces interfaces;
        return Cli::present_interface_ip(command.name, interfaces.addresses(command.name));
    }

    int execute_command(const Cli::DnsResolveCommand &command) {
        const auto raw_config = Config::load_config(command.config_path);
        // Normalise only — this command deliberately performs no validation,
        // and binds no I/O cancellation (legacy one-shot behaviour).
        auto dispatcher =
            DnsResolverFactory::create(Config::normalize(raw_config).resolver, {}, ResolverCatalog::with_builtins());
        return Cli::present_dns_resolve(Diagnostics::dns_resolve(dispatcher, command.host, command.type));
    }

    int execute_command(const Cli::DnsResolverCommand &command) {
        return Cli::present_dns_resolver(Config::load_config(command.config_path).resolver);
    }

    int execute_command(const Cli::ConfigShowCommand &command) {
        return Cli::present_config_show(Config::load_config(command.config_path));
    }

    int execute_command(const Cli::ConfigTestCommand &command) {
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

            DriverCatalog driver_catalog;
            DriverLoader::load(driver_catalog, config->driver);
            const SystemNetworkInterfaces interfaces;
            if (const auto env = validate_environment(*config, driver_catalog, interfaces); !env.has_value()) {
                return Cli::present_config_test(
                    {.quiet = command.quiet,
                     .error = Error{.kind = Error::Kind::VERIFICATION, .message = env.error().front().message}});
            }

            return Cli::present_config_test({.quiet = command.quiet, .error = std::nullopt});
        } catch (const ConfigVerificationException &e) {
            return Cli::present_config_test(
                {.quiet = command.quiet, .error = Error{.kind = Error::Kind::VERIFICATION, .message = e.what()}});
        } catch (const YaddnscException &e) {
            return Cli::present_config_test(
                {.quiet = command.quiet, .error = Error{.kind = Error::Kind::FATAL, .message = e.what()}});
        } catch (const std::exception &e) {
            return Cli::present_config_test(
                {.quiet = command.quiet, .error = Error{.kind = Error::Kind::GENERIC, .message = e.what()}});
        }
    }

    int execute_command(const Cli::InfoCommand &) {
        return Cli::present_info();
    }
} // anonymous namespace

int Composition::dispatch(const Cli::Command &command) {
    // RUN keeps its own error boundary in main() (fatal log lines).
    if (const auto *run = std::get_if<Cli::RunCommand>(&command)) {
        return run_command(*run);
    }

    // Diagnostic commands: any escaping failure is presented as
    // "Error: <what>" — the legacy catch-all wording.
    try {
        return std::visit(
            []<typename CommandT>(const CommandT &cmd) {
                // The RunCommand alternative never reaches the visitor (it is
                // guarded above); the branch only completes the overload set.
                if constexpr (std::is_same_v<CommandT, Cli::RunCommand>) {
                    return run_command(cmd);
                } else {
                    return execute_command(cmd);
                }
            },
            command);
    } catch (const std::exception &e) {
        return Cli::present_error(e);
    }
}
