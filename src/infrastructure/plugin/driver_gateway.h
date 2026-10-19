#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_DRIVER_GATEWAY_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_DRIVER_GATEWAY_H

#include <chrono>
#include <expected>  // IWYU pragma: keep — compiled standalone as a header check, so the include must stay
#include <memory>
#include <string>
#include <string_view>

#include "application/ports/gateway.h"
#include "coro/host.hpp"
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

/// Application-port adapter selecting the module's fixed sync/async execution
/// group and enforcing capabilities before create. Every cycle uses a fresh
/// instance; cycles of one module may run concurrently. All methods are
/// loop-thread only. Protocol internals live under sync/, async/ and detail/.
class DriverGateway final : public app::GatewayPort {
public:
    struct Options {
        http::Options http{};                                ///< HTTP policy for provider requests.
        std::chrono::milliseconds bridge_wait_budget{5000};  ///< Per-exchange budget.
    };

    /// Borrows catalog for the gateway lifetime. Logger and loop must outlive
    /// all submitted cycles, even abandoned ones, through coro::run drain.
    /// bridge_group is the root nursery owning those cycles and their HTTP.
    DriverGateway(const DriverCatalog& catalog, const app::LoggerPort& logger, coro::Loop& loop,
                  coro::Nursery& bridge_group);
    DriverGateway(const DriverCatalog& catalog, const app::LoggerPort& logger, coro::Loop& loop,
                  coro::Nursery& bridge_group, Options options);

    DriverGateway(const DriverGateway&) = delete;
    DriverGateway& operator=(const DriverGateway&) = delete;
    DriverGateway(DriverGateway&&) = delete;
    DriverGateway& operator=(DriverGateway&&) = delete;
    ~DriverGateway() noexcept override = default;

    /// Run a fresh-instance update. Operational failures are DriverError values;
    /// host defects propagate. Caller cancellation abandons its wait, not the
    /// root-owned worker/HTTP drain. Unstarted cancelled work skips plugin entry;
    /// late host defects remain observable. Sync runs a whole cycle on one
    /// worker; async offloads individual joined steps and waits for HTTP on-loop.
    [[nodiscard]] coro::Task<std::expected<void, domain::DriverError>> update(
        std::string driver_name, domain::DriverUpdateCommand command) override;

    /// Whether the loaded execution group exports its optional validate entry.
    /// Unknown drivers return false; missing validation does not prevent updates.
    [[nodiscard]] bool supports_validate(std::string_view driver_name) const override;

    /// Fresh-instance parameter validation, without HTTP. Missing validation
    /// fails as a value. Uses update's cancellation/lifetime contract; plugin-only
    /// CANCELLED is a validation failure, not a host cancellation request.
    [[nodiscard]] coro::Task<std::expected<void, domain::DriverError>> validate_config(
        std::string driver_name, std::string driver_param_json) override;

    /// Borrowed synchronous Host Services bridge, valid for the gateway lifetime.
    [[nodiscard]] Bridge& bridge() noexcept { return *bridge_; }

private:
    const DriverCatalog& catalog_;
    const app::LoggerPort& logger_;
    Options options_;
    std::shared_ptr<Bridge> bridge_;
    coro::NurseryToken cycle_group_;
};

}  // namespace plugin

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_DRIVER_GATEWAY_H
