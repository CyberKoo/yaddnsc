//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_CORE_CPP_DRIVER_GATEWAY_H
#define YADDNSC_CORE_CPP_DRIVER_GATEWAY_H

#include <functional>
#include <memory>

#include "application/ports/driver_gateway.h"

class HttpClient;
class DriverManagerBase;

/// Factory type for creating HttpClient instances on demand.
using HttpClientFactory = std::function<std::unique_ptr<HttpClient>()>;

/// CppDriverGateway — DriverGateway implementation over the current in-process
/// C++ Driver plugins (Phase 2 adapter).
///
/// Looks the driver up in the DriverManager, builds a fresh HttpClient per
/// update call (same lifecycle as the legacy per-task client), and translates
/// the legacy outcomes into DriverError values:
///   - DriverNotFoundException  → NOT_FOUND
///   - execute() == false       → UPDATE_FAILED (the driver already logged why)
///   - any other exception      → UNKNOWN (e.g. ParamParseException, which
///                                previously escaped to the Updater catch-all)
///
/// Phase 4 replaces this adapter with the C-ABI plugin host without changing
/// the port signature.
///
/// @note Thread-safe: update() is const; the shared Driver instances are
///       required to be thread-safe by the interface/driver.h contract.
class CppDriverGateway final : public DriverGateway {
public:
    /// @param driver_manager  Loaded-driver registry (non-owning; must outlive
    ///                        the gateway — it does in Manager::Impl).
    /// @param http_factory    Factory creating one HttpClient per update call.
    CppDriverGateway(const DriverManagerBase &driver_manager, HttpClientFactory http_factory);

    [[nodiscard]] std::expected<void, domain::DriverError>
    update(std::string_view driver_name, const DriverUpdateCommand &command) const override;

private:
    const DriverManagerBase &driver_manager_;
    HttpClientFactory http_factory_;
};

#endif // YADDNSC_CORE_CPP_DRIVER_GATEWAY_H
