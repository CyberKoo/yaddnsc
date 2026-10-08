//
// plugin — the coroutine driver gateway over the v1 alpha C ABI plugin host.
//

#ifndef YADDNSC_PLUGIN_CORO_DRIVER_GATEWAY_H
#define YADDNSC_PLUGIN_CORO_DRIVER_GATEWAY_H

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include <expected>

#include "application/coro/ports.h"
#include "application/ports/driver_gateway.h"
#include "domain/error/error.h"
#include "infrastructure/coro/serial_lane.hpp"
#include "infrastructure/coro/task.hpp"
#include "infrastructure/net/http/types.h"
#include "infrastructure/plugin/coro/bridge.h"

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
/// Ordering: every driver name owns one SerialLane, so its create/update/destroy
/// cycles execute in submission order. Under abandon an abandoned cycle still
/// holds its place on the lane, so a later cycle waits behind it.
///
/// Status mapping (yaddnsc_status → domain::DriverError) is unchanged from the
/// synchronous gateway: see abi_driver_gateway.cpp. The capability gate runs
/// before create() and returns UPDATE_FAILED directly.
///
/// Implements app::GatewayPort, so the application layer reaches it through the
/// port instead of this concrete type.
///
/// Thread safety: every method is loop-thread only. Retire a driver's lane only
/// after it is unloaded and no further cycles will be submitted for that name.
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

    /// Perform one update through the lane of `driver_name`.
    ///
    /// Failure: DriverError values; a defect (allocation, a host bug) escapes as
    /// an exception. Cancellation: the awaiting scope abandons the call — this
    /// returns CANCELLED while the cycle keeps its place on the lane and runs to
    /// completion on the worker.
    [[nodiscard]] coro::Task<std::expected<void, domain::DriverError>> update(std::string driver_name,
                                                                              DriverUpdateCommand command) override;

    /// Validate one subdomain's driver_params JSON against the driver's schema
    /// without performing an update (the host's `config test` path). Runs the
    /// same create → validate → destroy cycle on the name's lane. The optional
    /// yaddnsc_driver_validate entry stays optional: a plugin that omits it
    /// still loads and can update, but this call fails because the host cannot
    /// confirm driver_params.
    [[nodiscard]] coro::Task<std::expected<void, domain::DriverError>> validate_config(std::string driver_name,
                                                                                       std::string driver_param_json);

    /// Drop a driver's lane. Call after the driver is unloaded and no further
    /// cycles will be submitted for that name; in-flight and queued cycles hold
    /// their own share of the lane, so they still drain and report.
    void retire(std::string_view driver_name) noexcept;

    /// Number of live lanes; one per driver name that has been used.
    [[nodiscard]] std::size_t lane_count() const noexcept { return lanes_.size(); }

    /// The host-service HTTP bridge. Borrowed; lives as long as the gateway.
    [[nodiscard]] Bridge& bridge() noexcept { return *bridge_; }

private:
    [[nodiscard]] std::shared_ptr<coro::SerialLane> lane_for(std::string_view driver_name);

    const DriverCatalog& catalog_;
    const Logger& logger_;
    std::shared_ptr<Bridge> bridge_;
    std::unordered_map<std::string, std::shared_ptr<coro::SerialLane>> lanes_;
};

}  // namespace plugin

#endif  // YADDNSC_PLUGIN_CORO_DRIVER_GATEWAY_H
