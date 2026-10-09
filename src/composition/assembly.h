#ifndef YADDNSC_COMPOSITION_ASSEMBLY_H
#define YADDNSC_COMPOSITION_ASSEMBLY_H

#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <spdlog/common.h>

#include "domain/config/runtime_config.h"
#include "domain/error/error.h"
#include "infrastructure/http/types.h"
#include "infrastructure/plugin/driver_catalog.h"
#include "infrastructure/plugin/driver_gateway.h"

namespace Composition::internal {
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
}  // namespace Composition::internal
#endif  // YADDNSC_COMPOSITION_ASSEMBLY_H
