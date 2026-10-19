#include "run.h"

#include <cstdlib>
#include <functional>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include <memory>
#include <string>
#include <utility>

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
#include "coro/task.hpp"
#include "domain/config/dns_config.h"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include "domain/config/runtime_config.h"
#include "domain/error/error.h"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include "infrastructure/config/config_exception.h"
#include "infrastructure/dns/resolver_adapter.h"
#include "infrastructure/http/types.h"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include "infrastructure/ip_source/adapter.h"
#include "infrastructure/ip_source/system_network_interfaces.h"
#include "infrastructure/logging/spdlog_logger.h"
#include "infrastructure/plugin/driver_catalog.h"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include "support/exception.h"

namespace coro {
class Nursery;
}  // namespace coro

namespace Composition {
/// The caller owns the signal handler through command execution and log drain.
int execute_command(const Cli::RunCommand& command, const coro::StopSignals& signals) {
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

        // The validator performs the one interface enumeration this startup makes.
        if (const auto env = app::validate_environment(*runtime, driver_catalog, interfaces); !env.has_value()) {
            SPDLOG_CRITICAL("{}", internal::format_config_errors(env.error()));
            return EXIT_FAILURE;
        }

        const auto tls_context = internal::make_default_tls_context();
        if (signals.requested()) {
            SPDLOG_INFO("Received signal during startup, shutting down...");
            return EXIT_SUCCESS;
        }
        const auto http_options = internal::make_coro_http_options(runtime->resolver, tls_context);
        const auto dispatcher = internal::make_dispatcher(runtime->resolver, tls_context);
        dns::DispatcherResolverPort resolver_port{*dispatcher};
        ipsource::IpSourceAdapter ip_source{http_options};

        // The loop object is created here, but nothing runs until coro::run below.
        auto loop = internal::make_traced_loop();
        coro::Loop& run_loop = *loop;

        // The driver gateway needs the main loop's bridge nursery, which only
        // exists inside the run; the main loop calls make_gateway with its root
        // group and owns the gateway for the whole run.
        const app::RuntimeServices services{
            .resolver = resolver_port,
            .ip_source = ip_source,
            .logger = logger,
            .make_gateway = [&driver_catalog, &logger, &run_loop,
                             &http_options](coro::Nursery& group) -> std::unique_ptr<app::GatewayPort> {
                return internal::make_driver_gateway(driver_catalog, logger, run_loop, group, http_options);
            },
            .stop_requested = [&signals] { return signals.requested(); },
            .wait_for_stop = [&signals] { return signals.wait(); },
            .policy = internal::UPDATE_POLICY,
        };
        return internal::run_exit_code(coro::run(run_loop, app::main_loop(runtime, services)));
    } catch (const ConfigException& e) {
        SPDLOG_CRITICAL("{}", e.what());
        return EXIT_FAILURE;
    } catch (const YaddnscException& e) {
        SPDLOG_CRITICAL("Fatal error {}: {}", e.get_name(), e.what());
        return EXIT_FAILURE;
    }
}

}  // namespace Composition
