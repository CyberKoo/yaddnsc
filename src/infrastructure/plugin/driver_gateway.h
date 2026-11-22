#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_DRIVER_GATEWAY_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_DRIVER_GATEWAY_H

#include <chrono>
#include <expected>
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
struct SessionDependencies;

/// Application-port adapter over immutable driver packages and their protocol
/// sessions. Capability checks happen here; protocol and delivery lifecycle
/// live in DriverSession and CycleRunner respectively.
class DriverGateway final : public app::GatewayPort {
public:
    struct Options {
        http::Options http{};
        std::chrono::milliseconds bridge_wait_budget{5000};
    };

    DriverGateway(const DriverCatalog& catalog, const app::LoggerPort& logger, coro::Loop& loop,
                  coro::Nursery& cycle_nursery);
    DriverGateway(const DriverCatalog& catalog, const app::LoggerPort& logger, coro::Loop& loop,
                  coro::Nursery& cycle_nursery, Options options);

    DriverGateway(const DriverGateway&) = delete;
    DriverGateway& operator=(const DriverGateway&) = delete;
    DriverGateway(DriverGateway&&) = delete;
    DriverGateway& operator=(DriverGateway&&) = delete;
    ~DriverGateway() noexcept override = default;

    [[nodiscard]] coro::Task<std::expected<void, domain::DriverError>> update(
        std::string driver_name, domain::DriverUpdateCommand command) override;

    [[nodiscard]] bool supports_validate(std::string_view driver_name) const override;
    [[nodiscard]] coro::Task<std::expected<void, domain::DriverError>> validate_config(
        std::string driver_name, std::string driver_param_json) override;

private:
    [[nodiscard]] SessionDependencies dependencies() const;

    const DriverCatalog& catalog_;
    const app::LoggerPort& logger_;
    coro::Loop& loop_;
    coro::Nursery& cycle_nursery_;
    coro::NurseryToken cycle_nursery_token_;
    Options options_;
};

}  // namespace plugin

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_DRIVER_GATEWAY_H
