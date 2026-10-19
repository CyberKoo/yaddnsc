#ifndef YADDNSC_COMPOSITION_ASSEMBLY_H
#define YADDNSC_COMPOSITION_ASSEMBLY_H

#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

#include "application/services.h"
#include "application/tasks/main_loop.h"  // IWYU pragma: keep — compiled standalone as a header check, so the include must stay
#include "domain/config/runtime_config.h"
#include "infrastructure/dns/policy.h"
#include "infrastructure/http/types.h"
#include "infrastructure/network/tls/context.h"
#include "infrastructure/plugin/driver_catalog.h"
#include "infrastructure/plugin/driver_gateway.h"

namespace domain {
struct ConfigError;
}  // namespace domain

namespace dns {
class Dispatcher;
}  // namespace dns

namespace coro {
class Loop;
class Nursery;
}  // namespace coro

namespace app {
class LoggerPort;
}  // namespace app

namespace Composition::internal {
/// Production defaults shared by the command assembly paths.
inline constexpr dns::Policy DNS_POLICY{};
/// Per-record budget for one driver's configuration validation. Injected, not
/// defaulted downstream, so `run` and `config test` apply the same deadline to
/// the same plugin call.
inline constexpr std::chrono::milliseconds CONFIG_VALIDATION_BUDGET{5000};
inline constexpr app::UpdatePolicy UPDATE_POLICY{.driver_validation_budget = CONFIG_VALIDATION_BUDGET};
/// Internal startup helpers; call on the main thread before coro::run.
/// Configuration and loader failures abort the command; allocation defects propagate.
[[nodiscard]] spdlog::level::level_enum to_log_level(std::string_view level);
[[nodiscard]] domain::RuntimeConfig load_runtime_config(const std::string& path);
[[nodiscard]] DriverCatalog load_catalog(const domain::RuntimeConfig& config);
[[nodiscard]] std::string format_config_errors(std::span<const domain::ConfigError> errors);
[[nodiscard]] std::shared_ptr<const net::TlsContext> make_default_tls_context();
[[nodiscard]] http::Options make_coro_http_options(const domain::ResolverSettings& resolver,
                                                   std::shared_ptr<const net::TlsContext> tls_context);
[[nodiscard]] plugin::DriverGateway::Options make_gateway_options(const http::Options& options);
/// The single dispatcher recipe: DNS_POLICY, the shared trust context, and
/// the factory's std::invalid_argument translated into a ConfigException. The
/// factory checks protect direct callers too, so drift from static validation
/// stays a configuration failure instead of an internal exception type
/// escaping into a command's presentation.
[[nodiscard]] std::unique_ptr<dns::Dispatcher> make_dispatcher(const domain::ResolverSettings& resolver,
                                                               const std::shared_ptr<const net::TlsContext>& tls_context);
/// A run loop with trace diagnostics already wired; the caller owns it until
/// coro::run returns.
[[nodiscard]] std::unique_ptr<coro::Loop> make_traced_loop();
/// The single driver-gateway recipe: one catalog, one logger, one loop and the
/// shared HTTP policy. `bridge_group` owns the gateway's bridge children, so
/// the returned owner must live at least as long as that nursery.
[[nodiscard]] std::unique_ptr<plugin::DriverGateway> make_driver_gateway(const DriverCatalog& catalog,
                                                                         const app::LoggerPort& logger,
                                                                         coro::Loop& loop, coro::Nursery& bridge_group,
                                                                         const http::Options& http_options);
/// The process exit status for a finished run. The loop reports what happened;
/// deciding what the process returns is the command handler's policy: only a
/// rejected configuration and the unexpected end of every update task fail,
/// while a stop request is the normal way out.
[[nodiscard]] int run_exit_code(app::MainLoopOutcome outcome) noexcept;
}  // namespace Composition::internal
#endif  // YADDNSC_COMPOSITION_ASSEMBLY_H
