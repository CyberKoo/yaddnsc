//
// plugin — the coroutine driver gateway over the v1 C ABI plugin host.
//

#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_DRIVER_GATEWAY_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_DRIVER_GATEWAY_H

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include "application/ports/gateway.h"
#include "infrastructure/http/types.h"

class DriverCatalog;

namespace app {
class LoggerPort;
}

namespace coro {
class Loop;
class Nursery;
}  // namespace coro

namespace plugin {
class Bridge;

/// DriverGateway — coroutine DriverGateway over the v1 C ABI plugin host.
///
/// Every update() call runs a full cycle on a fresh driver instance, but the
/// loader fixes one execution group per module and the two are driven
/// differently:
///
/// - async mode (recommended): the protocol runs on the loop. Each plugin entry
///   call is a shielded offload-pool step; a step that returns PENDING releases
///   the worker, so a pending HTTP exchange occupies no pool capacity. Accepted
///   HTTP commands start only after the step returns, and completions come back
///   as events.
/// - synchronous mode (supported, not the recommended target for new code): the
///   whole cycle runs on one offload worker, and host HTTP reaches the loop
///   through the Bridge, so an exchange blocks that worker.
///
/// Both modes bind the instance to a per-call services context.
///
/// Concurrency: cycles run concurrently, including cycles of one driver — the
/// ABI guarantees that distinct driver instances may run in parallel, and every
/// cycle creates a fresh instance. In async mode the bound is the number of
/// concurrent HTTP operations a cycle may admit; in synchronous mode it is the
/// offload pool's worker count.
///
/// Status mapping: yaddnsc_status → domain::DriverError. The capability gate
/// runs before create() and returns UPDATE_FAILED directly.
///
/// Implements app::GatewayPort, so the application layer reaches it through the
/// port instead of this concrete type.
///
/// Thread safety: every method is loop-thread only.
class DriverGateway final : public app::GatewayPort {
public:
    /// Construction-time policy.
    struct Options {
        /// HTTP policy for the loop-side client (hostname resolver, TLS, factory).
        http::Options http{};
        /// Upper bound on one bridged exchange, in milliseconds.
        std::chrono::milliseconds bridge_wait_budget{5000};
    };

    /// @param catalog       Loaded-plugin registry (non-owning; must outlive
    ///                      the gateway — it does in the composition root).
    /// @param logger        Log port receiving plugin log records (non-owning);
    ///                      must outlive all submitted cycles, including abandoned
    ///                      workers: keep it alive until coro::run returns.
    /// @param loop          Loop that runs the bridge's exchanges.
    /// @param bridge_group  Structured scope for the bridge's loop-side
    ///                      coroutines; the application binds its root group
    ///                      here so bridge work is never detached.
    /// @param options       Exchange policy.
    DriverGateway(const DriverCatalog& catalog, const app::LoggerPort& logger, coro::Loop& loop,
                  coro::Nursery& bridge_group);
    DriverGateway(const DriverCatalog& catalog, const app::LoggerPort& logger, coro::Loop& loop,
                  coro::Nursery& bridge_group, Options options);

    DriverGateway(const DriverGateway&) = delete;
    DriverGateway& operator=(const DriverGateway&) = delete;
    DriverGateway(DriverGateway&&) = delete;
    DriverGateway& operator=(DriverGateway&&) = delete;

    ~DriverGateway() noexcept override = default;

    /// Perform one update cycle of `driver_name`.
    ///
    /// Sync modules run the whole cycle on one offload worker; async modules run
    /// the protocol on the loop with each entry call offloaded as a shielded step.
    ///
    /// Failure: DriverError values; a defect (allocation, a host bug) escapes as
    /// an exception. Cancellation: the awaiting scope abandons the call — this
    /// throws coro::Cancelled while a cycle that already started runs to
    /// completion (in async mode, after an UPDATE_CANCELLED event lets the
    /// plugin retire); a cycle not yet started is dropped and never enters the
    /// driver. A root-owned cycle drains the joined worker and every HTTP child;
    /// host defects remain observable even after caller cancellation.
    [[nodiscard]] coro::Task<std::expected<void, domain::DriverError>> update(
        std::string driver_name, domain::DriverUpdateCommand command) override;

    /// app::GatewayPort: whether the plugin exports the optional validate entry
    /// matching its selected execution group (`yaddnsc_driver_validate` in sync
    /// mode, `yaddnsc_driver_async_validate` in async mode). A driver that does
    /// not still loads and updates; this is what lets the caller pick between
    /// requiring the check (`config test`) and warning past it (`run`). An
    /// unknown driver answers false — there is nothing to validate against.
    [[nodiscard]] bool supports_validate(std::string_view driver_name) const override;

    /// Validate one subdomain's driver_params JSON against the driver's schema
    /// without performing an update (the host's `config test` path). Runs the
    /// same create → validate → destroy cycle as update(), on the thread and in
    /// the execution mode the module was loaded with. The optional validate
    /// entry stays optional: a plugin that omits it still loads and can update,
    /// but this call fails because the host cannot confirm driver_params. A
    /// plugin-reported CANCELLED on a live host scope is a validation error
    /// value, not a request to cancel the host. Uses the same
    /// abandon/worker-lifetime contract as update().
    [[nodiscard]] coro::Task<std::expected<void, domain::DriverError>> validate_config(
        std::string driver_name, std::string driver_param_json) override;

    /// The host-service HTTP bridge. Borrowed; lives as long as the gateway.
    [[nodiscard]] Bridge& bridge() noexcept { return *bridge_; }

private:
    const DriverCatalog& catalog_;
    const app::LoggerPort& logger_;
    Options options_;
    std::shared_ptr<Bridge> bridge_;
};

}  // namespace plugin

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_DRIVER_GATEWAY_H
