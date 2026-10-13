#include "diagnostic_commands.h"

#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <coroutine>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not

#include <expected>
#include <magic_enum/magic_enum.hpp>
#include <spdlog/spdlog.h>

#include "application/diagnostics.h"
#include "application/environment_validator.h"
#include "cli/command.h"
#include "cli/presenter.h"
#include "composition/assembly.h"
#include "coro/group.hpp"
#include "coro/loop.h"
#include "coro/run.hpp"
#include "coro/task.hpp"
#include "domain/config/dns_config.h"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include "domain/config/runtime_config.h"
#include "domain/error/error.h"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include "infrastructure/config/config.h"
#include "infrastructure/config/config_exception.h"
#include "infrastructure/dns/dispatcher.h"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include "infrastructure/dns/factory.h"
#include "infrastructure/dns/resolver_adapter.h"
#include "infrastructure/http/types.h"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include "infrastructure/ip_source/system_network_interfaces.h"
#include "infrastructure/logging/spdlog_logger.h"
#include "infrastructure/plugin/driver_catalog.h"
#include "infrastructure/plugin/driver_gateway.h"
#include "support/exception.h"

namespace coro {
class TaskGroup;
}  // namespace coro

namespace Composition {
int execute_command(const Cli::DriverListCommand& command) {
    const auto catalog = internal::load_catalog(internal::load_runtime_config(command.config_path));
    return Cli::present_driver_list(app::list_drivers(catalog));
}

int execute_command(const Cli::DriverInfoCommand& command) {
    const auto catalog = internal::load_catalog(internal::load_runtime_config(command.config_path));
    return Cli::present_driver_info(command.name, catalog.describe(command.name));
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
    const auto config = internal::load_runtime_config(command.config_path);
    const auto tls_context = internal::make_default_tls_context();
    const auto dispatcher = [&] {
        try {
            return dns::make_dispatcher(config.resolver, config.resolver.bootstrap_servers, tls_context,
                                        internal::DNS_POLICY);
        } catch (const std::invalid_argument& error) {
            // Same translation as the run command: factory checks protect
            // direct callers too, so drift from static validation is a
            // configuration failure rather than an internal exception type
            // escaping into this command's presentation.
            throw ConfigException(error.what());
        }
    }();
    dns::DispatcherResolverPort resolver_port{*dispatcher};
    // A plain root preserves the default Ctrl-C disposition for this one-shot command.
    coro::Loop loop;
    internal::install_loop_trace_sink(loop);
    const auto outcome =
        coro::run(loop, app::dns_resolve_command(resolver_port, command.host, command.type, std::chrono::seconds{30}));
    return Cli::present_dns_resolve(outcome);
}

int execute_command(const Cli::DnsResolverCommand& command) {
    // Intentionally show the raw DTO: inspection must work even when runtime validation fails.
    const auto resolver = Config::load_config(command.config_path).resolver;
    return Cli::present_dns_resolver(resolver.use_custom_servers, magic_enum::enum_name(resolver.strategy),
                                     resolver.servers);
}

int execute_command(const Cli::ConfigShowCommand& command) {
    // Intentionally load only the raw DTO; showing configuration needs no runtime validation.
    return Cli::present_config_show(Config::redacted_json(Config::load_config(command.config_path)));
}

int execute_command(const Cli::ConfigTestCommand& command) {
    using Error = app::ConfigTestError;

    // <scope> is unavailable on supported libstdc++ versions.
    struct RestoreLogLevel {
        spdlog::level::level_enum previous = spdlog::get_level();

        ~RestoreLogLevel() noexcept { spdlog::set_level(previous); }
    } restore_level;

    if (command.quiet) {
        spdlog::set_level(spdlog::level::off);
    }

    try {
        const auto config = internal::load_runtime_config(command.config_path);

        auto driver_catalog = internal::load_catalog(config);
        const SystemNetworkInterfaces interfaces;
        if (const auto env = app::validate_environment(config, driver_catalog, interfaces); !env.has_value()) {
            return Cli::present_config_test({.quiet = command.quiet,
                                             .error = Error{.kind = Error::Kind::VERIFICATION,
                                                            .message = internal::format_config_errors(env.error())}});
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
        // validate_config runs in the VALIDATE phase, so the plugin cannot
        // reach http_exchange here and no TLS context is needed.
        const auto http_options = internal::make_coro_http_options(config.resolver, nullptr);
        coro::Loop loop;
        internal::install_loop_trace_sink(loop);
        coro::run(loop, coro::supervisor_group([&config, &driver_catalog, &logger, &loop,
                                                &http_options](coro::TaskGroup& group) -> coro::Task<void> {
                      plugin::DriverGateway gateway(driver_catalog, logger, loop, group,
                                                    internal::make_gateway_options(http_options));
                      // Keep gateway alive through child cleanup, including a
                      // validation defect or host cancellation.
                      co_await group.with_children([&]() -> coro::Task<void> {
                          // `config test` exists to answer whether the
                          // configuration is good, so a driver that cannot be
                          // checked is a failure, not a warning.
                          const auto outcome = co_await app::validate_driver_configs(
                              gateway, config, app::ConfigValidationMode::REQUIRE_VALIDATION,
                              internal::CONFIG_VALIDATION_BUDGET);
                          if (!outcome.rejection.empty()) {
                              throw ConfigException(outcome.rejection);
                          }
                      });
                  }));

        return Cli::present_config_test({.quiet = command.quiet, .error = std::nullopt});
    } catch (const ConfigException& e) {
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
}  // namespace Composition
