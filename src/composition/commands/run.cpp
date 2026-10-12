#include "run.h"

#include <cstdlib>
#include <functional>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <expected>
#include <spdlog/spdlog.h>

#include "application/environment_validator.h"
#include "application/ports/gateway.h"  // IWYU pragma: keep — make_gateway yields std::unique_ptr<GatewayPort>; clangd sees no spelled use
#include "application/services.h"
#include "application/tasks/main_loop.h"
#include "cli/command.h"
#include "composition/assembly.h"
#include "coro/loop.h"
#include "coro/run.hpp"
#include "coro/stop_signals.h"
#include "domain/config/dns_config.h"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include "domain/config/runtime_config.h"
#include "domain/error/error.h"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include "infrastructure/config/config_exception.h"
#include "infrastructure/dns/dispatcher.h"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include "infrastructure/dns/factory.h"
#include "infrastructure/dns/resolver_adapter.h"
#include "infrastructure/http/types.h"
#include "infrastructure/ip_source/adapter.h"
#include "infrastructure/ip_source/system_network_interfaces.h"
#include "infrastructure/logging/spdlog_logger.h"
#include "infrastructure/plugin/driver_catalog.h"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include "infrastructure/plugin/driver_gateway.h"
#include "support/exception.h"
#include "support/fmt.hpp"
#include "yaddnsc/util/format.hpp"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not

namespace coro {
class TaskGroup;
}  // namespace coro

namespace Composition {
/// The caller owns the signal handler through command execution and log drain.
int execute_command(const Cli::RunCommand& command, const coro::StopSignals& signals) {
    if (!command.log_level.empty()) {
        const auto level = internal::to_log_level(command.log_level);
        spdlog::set_level(level);
        if (level <= spdlog::level::debug) {
            // Debug runs are foreground sessions: flush every record so the
            // output survives a crash and appears live through pipes and files.
            spdlog::flush_on(spdlog::level::debug);
        }
        SPDLOG_DEBUG("Log level set to {}", command.log_level);
    } else if (command.verbose) {
        spdlog::set_level(spdlog::level::debug);
        spdlog::flush_on(spdlog::level::debug);
        SPDLOG_DEBUG("Verbose mode enabled");
    }

    try {
        auto runtime_config = internal::load_runtime_config(command.config_path);
        if (signals.requested()) {
            SPDLOG_INFO("Received signal during startup, shutting down...");
            return EXIT_SUCCESS;
        }

        const SpdlogLogger logger;
        const SystemNetworkInterfaces interfaces;
        const auto runtime = std::make_shared<const domain::RuntimeConfig>(std::move(runtime_config));
        auto driver_catalog = internal::load_catalog(*runtime);
        if (signals.requested()) {
            SPDLOG_INFO("Received signal during startup, shutting down...");
            return EXIT_SUCCESS;
        }

        if (const auto env = app::validate_environment(*runtime, driver_catalog, interfaces); !env.has_value()) {
            SPDLOG_CRITICAL("{}", internal::format_config_errors(env.error()));
            return EXIT_FAILURE;
        }
        SPDLOG_INFO("All available interfaces: {}", fmt::join(interfaces.names(), ", "));

        const auto tls_context = internal::make_default_tls_context();
        if (signals.requested()) {
            SPDLOG_INFO("Received signal during startup, shutting down...");
            return EXIT_SUCCESS;
        }
        const auto http_options = internal::make_coro_http_options(runtime->resolver, tls_context);
        const auto dispatcher = [&] {
            try {
                return dns::make_dispatcher(runtime->resolver, runtime->resolver.bootstrap_servers, tls_context,
                                            internal::DNS_POLICY);
            } catch (const std::invalid_argument& error) {
                // Factory checks protect direct callers too. Treat any drift
                // from static validation as configuration failure at startup.
                throw ConfigException(error.what());
            }
        }();
        dns::DispatcherResolverPort resolver_port{*dispatcher};
        ipsource::IpSourceAdapter ip_source{http_options};

        // The loop object is created here, but nothing runs until coro::run below.
        coro::Loop loop;
        internal::install_loop_trace_sink(loop);

        // The driver gateway needs the main loop's bridge TaskGroup, which only
        // exists inside the run; the main loop calls make_gateway with its root
        // group and owns the gateway for the whole run.
        const app::RuntimeServices services{
            .resolver = resolver_port,
            .ip_source = ip_source,
            .logger = logger,
            .make_gateway =
                [&driver_catalog, &logger, &loop, &http_options](coro::TaskGroup& group) {
                    return std::make_unique<plugin::DriverGateway>(driver_catalog, logger, loop, group,
                                                                   internal::make_gateway_options(http_options));
                },
            .stop_requested = [&signals] { return signals.requested(); },
            .wait_for_stop = [&signals] { return signals.wait(); },
            .policy = internal::UPDATE_POLICY,
        };
        return coro::run(loop, app::main_loop(runtime, services));
    } catch (const ConfigException& e) {
        SPDLOG_CRITICAL("{}", e.what());
        return EXIT_FAILURE;
    } catch (const YaddnscException& e) {
        SPDLOG_CRITICAL("Fatal error {}: {}", e.get_name(), e.what());
        return EXIT_FAILURE;
    }
}

}  // namespace Composition
