#include "run.h"

#include <spdlog/spdlog.h>
#include <functional>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include <cstdlib>
#include <memory>
#include <utility>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "application/environment_validator.h"
#include "application/run_root.h"
#include "application/services.h"
#include "composition/assembly.h"
#include "domain/config/dns_config.h"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include "domain/error/error.h"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include "infrastructure/config/config_exception.h"
#include "coro/loop.h"
#include "coro/run.hpp"
#include "infrastructure/dns/dispatcher.h"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include "infrastructure/dns/factory.h"
#include "infrastructure/dns/resolver_adapter.h"
#include "infrastructure/ip_source/adapter.h"
#include "infrastructure/ip_source/system_network_interfaces.h"
#include "infrastructure/logging/async_logging.h"
#include "infrastructure/logging/spdlog_logger.h"
#include "infrastructure/plugin/driver_catalog.h"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include "infrastructure/plugin/driver_gateway.h"
#include "support/exception.h"
#include "support/fmt.hpp"
#include "cli/command.h"
#include "domain/config/runtime_config.h"
#include "infrastructure/http/types.h"
#include "yaddnsc/util/format.hpp"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include "application/ports/gateway.h"  // IWYU pragma: keep — make_gateway yields std::unique_ptr<GatewayPort>; clangd sees no spelled use

namespace coro {
class TaskGroup;
}  // namespace coro

namespace Composition {
/// Startup preparation and the run root.
///
/// Startup is synchronous on the main thread, before the loop exists: reading and
/// validating the configuration, dlopen-ing plugins, checking the environment and
/// building the trust context are all blocking file/loader I/O that has no reason
/// to touch the loop. Only the run root then runs inside coro::run.
int execute_command(const Cli::RunCommand& command) {
    if (!command.log_level.empty()) {
        spdlog::set_level(internal::to_log_level(command.log_level));
        SPDLOG_DEBUG("Log level set to {}", command.log_level);
    } else if (command.verbose) {
        spdlog::set_level(spdlog::level::debug);
        SPDLOG_DEBUG("Verbose mode enabled");
    }

    try {
        auto runtime_config = internal::load_runtime_config(command.config_path);

        const SpdlogLogger logger;
        const SystemNetworkInterfaces interfaces;
        const auto runtime = std::make_shared<const domain::RuntimeConfig>(std::move(runtime_config));
        auto driver_catalog = internal::load_catalog(*runtime);

        if (const auto env = app::validate_environment(*runtime, driver_catalog, interfaces); !env.has_value()) {
            SPDLOG_CRITICAL("{}", internal::format_config_errors(env.error()));
            return EXIT_FAILURE;
        }
        SPDLOG_INFO("All available interfaces: {}", fmt::format("{}", fmt::join(interfaces.names(), ", ")));

        const auto tls_context = internal::make_default_tls_context();
        const auto http_options = internal::make_coro_http_options(runtime->resolver, tls_context);
        const auto dispatcher =
            dns::make_dispatcher(runtime->resolver, runtime->resolver.bootstrap_servers, tls_context);
        dns::DispatcherResolverPort resolver_port{*dispatcher};
        ipsource::IpSourceAdapter ip_source{http_options};

        // The loop object is created here, but nothing runs until coro::run below.
        coro::Loop loop;

        // The driver gateway needs the runner's bridge TaskGroup, which only
        // exists inside the run; the run root calls make_gateway with its root
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
            .drain_logs = [] { logging::shutdown(); },
        };
        return coro::run(loop, app::run_root(runtime, services));
    } catch (const ConfigException& e) {
        SPDLOG_CRITICAL("{}", e.what());
        return EXIT_FAILURE;
    } catch (const YaddnscException& e) {
        SPDLOG_CRITICAL("Fatal error {}: {}", e.get_name(), e.what());
        return EXIT_FAILURE;
    }
}

}  // namespace Composition
