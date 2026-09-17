//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_ABI_DRIVER_GATEWAY_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_ABI_DRIVER_GATEWAY_H

#include <functional>
#include <memory>

#include "application/ports/driver_gateway.h"
#include "util/cancellation_token.hpp"

class HttpClient;
class DriverCatalog;
class Logger;

/// Factory type for creating HttpClient instances on demand.
using HttpClientFactory = std::function<std::unique_ptr<HttpClient>()>;

/// AbiDriverGateway — DriverGateway implementation over the v1 alpha C ABI
/// plugin host.
///
/// Every update() call runs the full create → update → destroy cycle on a
/// fresh driver instance bound to a per-call HostServicesContext (one arena,
/// one HttpClient). The module lease keeps the plugin code alive until the
/// instance is destroyed.
///
/// Status mapping (yaddnsc_status → domain::DriverError):
///   OK                     → success
///   RATE_LIMITED           → RATE_LIMITED (retry_after_seconds passed through)
///   CANCELLED              → CANCELLED
///   INVALID_CONFIG         → UNKNOWN (surfaces through the workflow catch-all
///   INTERNAL_ERROR            wording, as the legacy ParamParseException did)
///   NETWORK_ERROR          → UPDATE_FAILED (the plugin already logged the
///   AUTHENTICATION_FAILED     specific cause through Host Services logging)
///   UPSTREAM_REJECTED      → UPDATE_FAILED
///   UNSUPPORTED_RECORD     → UPDATE_FAILED
///   INVALID_RESPONSE       → UPDATE_FAILED
///   anything else          → UNKNOWN
///
/// @note Thread-safe: update() is const and every call owns its entire
///       instance/context chain.
class AbiDriverGateway final : public DriverGateway {
public:
    /// @param catalog       Loaded-plugin registry (non-owning; must outlive
    ///                      the gateway — it does in Manager::Impl).
    /// @param http_factory  Factory creating one HttpClient per update call.
    /// @param cancel_token  Manager-wide I/O cancellation signal.
    /// @param logger        Log port receiving plugin log records.
    AbiDriverGateway(const DriverCatalog &catalog, HttpClientFactory http_factory,
                     Utils::CancellationToken cancel_token, const Logger &logger);

    [[nodiscard]] std::expected<void, domain::DriverError>
    update(std::string_view driver_name, const DriverUpdateCommand &command) const override;

private:
    const DriverCatalog &catalog_;
    HttpClientFactory http_factory_;
    Utils::CancellationToken cancel_token_;
    const Logger &logger_;
};

#endif // YADDNSC_INFRASTRUCTURE_PLUGIN_ABI_DRIVER_GATEWAY_H
