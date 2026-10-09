//
// plugin — the coroutine driver gateway over the v1 alpha C ABI plugin host.
//

#ifndef YADDNSC_PLUGIN_DRIVER_GATEWAY_H
#define YADDNSC_PLUGIN_DRIVER_GATEWAY_H

#include <chrono>
#include <memory>
#include <string>

#include <expected>

#include "application/ports.h"
#include "domain/error/error.h"
#include "infrastructure/coro/task.hpp"
#include "infrastructure/net/http/types.h"
#include "infrastructure/plugin/bridge.h"

class DriverCatalog;
class Logger;

namespace coro {
class Loop;
class TaskGroup;
}  // namespace coro

namespace plugin {

/// DriverGateway — coroutine DriverGateway over the v1 alpha C ABI plugin host.
///
/// Every update() call runs the full create → update → destroy cycle on a fresh
/// driver instance bound to a per-call HostServicesContext (one arena, one
/// bridged HTTP exchange channel). The cycle itself runs on an offload worker
/// (the plugin C ABI is synchronous and must not run on the loop), and host HTTP
/// reaches the loop through the Bridge.
///
/// Concurrency: cycles run concurrently, including cycles of one driver — the
/// ABI guarantees that distinct driver instances may run in parallel, and every
/// cycle creates a fresh instance. The offload pool's worker count is the bound.
///
/// Status mapping (yaddnsc_status → domain::DriverError) is the legacy one: the
/// capability gate runs before create() and returns UPDATE_FAILED directly.
///
/// Implements app::GatewayPort, so the application layer reaches it through the
/// port instead of this concrete type.
///
/// Thread safety: every method is loop-thread only.
class DriverGateway final : public app::GatewayPort {
public:
    /// Construction-time policy.
    struct Options {
        /// HTTP policy for the loop-side client (bootstrap DNS, TLS, factory).
        http::Options http{};
        /// Upper bound on one bridged exchange, in milliseconds.
        std::chrono::milliseconds bridge_wait_budget{5000};
    };

    /// @param catalog       Loaded-plugin registry (non-owning; must outlive
    ///                      the gateway — it does in the composition root).
    /// @param logger        Log port receiving plugin log records (non-owning).
    /// @param loop          Loop that runs the bridge's exchanges.
    /// @param bridge_group  Structured scope for the bridge's loop-side
    ///                      coroutines; the application binds its root group
    ///                      here so bridge work is never detached.
    /// @param options       Exchange policy.
    DriverGateway(const DriverCatalog& catalog, const Logger& logger, coro::Loop& loop, coro::TaskGroup& bridge_group);
    DriverGateway(const DriverCatalog& catalog, const Logger& logger, coro::Loop& loop, coro::TaskGroup& bridge_group,
                  Options options);

    DriverGateway(const DriverGateway&) = delete;
    DriverGateway& operator=(const DriverGateway&) = delete;
    DriverGateway(DriverGateway&&) = delete;
    DriverGateway& operator=(DriverGateway&&) = delete;

    ~DriverGateway() noexcept override = default;

    /// Perform one update cycle of `driver_name` on an offload worker.
    ///
    /// Failure: DriverError values; a defect (allocation, a host bug) escapes as
    /// an exception. Cancellation: the awaiting scope abandons the call — this
    /// returns CANCELLED while a cycle that already started runs to completion
    /// on the worker; a cycle not yet started is dropped and never enters the
    /// driver.
    [[nodiscard]] coro::Task<std::expected<void, domain::DriverError>> update(std::string driver_name,
                                                                              domain::DriverUpdateCommand command) override;

    /// Validate one subdomain's driver_params JSON against the driver's schema
    /// without performing an update (the host's `config test` path). Runs the
    /// same create → validate → destroy cycle on an offload worker. The optional
    /// yaddnsc_driver_validate entry stays optional: a plugin that omits it
    /// still loads and can update, but this call fails because the host cannot
    /// confirm driver_params.
    [[nodiscard]] coro::Task<std::expected<void, domain::DriverError>> validate_config(std::string driver_name,
                                                                                       std::string driver_param_json);

    /// The host-service HTTP bridge. Borrowed; lives as long as the gateway.
    [[nodiscard]] Bridge& bridge() noexcept { return *bridge_; }

private:
    const DriverCatalog& catalog_;
    const Logger& logger_;
    std::shared_ptr<Bridge> bridge_;
};

}  // namespace plugin

#endif  // YADDNSC_PLUGIN_DRIVER_GATEWAY_H
