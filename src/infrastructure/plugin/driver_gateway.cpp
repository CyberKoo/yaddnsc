#include "driver_gateway.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include <yaddnsc/sdk/driver_abi.h>

#include "coro/cancel_scope.h"
#include "coro/scope.hpp"
#include "infrastructure/plugin/async/cycle.h"
#include "infrastructure/plugin/detail/cycle.h"
#include "infrastructure/plugin/driver_catalog.h"
#include "infrastructure/plugin/plugin_loader.h"
#include "infrastructure/plugin/sync/bridge.h"
#include "infrastructure/plugin/sync/cycle.h"
#include "support/fmt.hpp"
#include "yaddnsc/util/format.hpp"

namespace plugin {
namespace {

/// The host rejects unsupported records before any plugin entry is called.
[[nodiscard]] std::optional<uint64_t> required_capability(std::string_view record_type) noexcept {
    if (record_type == "A") {
        return YADDNSC_DRIVER_CAPABILITY_A;
    }
    if (record_type == "AAAA") {
        return YADDNSC_DRIVER_CAPABILITY_AAAA;
    }
    return std::nullopt;
}

[[nodiscard]] domain::DriverError unsupported_record(std::string_view driver_name, std::string_view record_type,
                                                     std::optional<uint64_t> required) {
    const std::string message =
        required.has_value()
            ? fmt::format("Driver '{}' does not support record type {} (missing capability {})", driver_name,
                          record_type, *required == YADDNSC_DRIVER_CAPABILITY_AAAA ? "AAAA" : "A")
            : fmt::format("Driver '{}' does not support record type {} (no v1 capability)", driver_name, record_type);
    return {domain::DriverError::Code::UPDATE_FAILED, message, 0};
}

}  // namespace

DriverGateway::DriverGateway(const DriverCatalog& catalog, const app::LoggerPort& logger, coro::Loop& loop,
                             coro::Nursery& bridge_group)
    : DriverGateway(catalog, logger, loop, bridge_group, Options{}) {}

DriverGateway::DriverGateway(const DriverCatalog& catalog, const app::LoggerPort& logger, coro::Loop& loop,
                             coro::Nursery& bridge_group, Options options)
    : catalog_(catalog), logger_(logger), options_(std::move(options)),
      bridge_(std::make_shared<Bridge>(loop, bridge_group, options_.http, options_.bridge_wait_budget)),
      cycle_group_(coro::nursery_token(bridge_group)) {}

coro::Task<std::expected<void, domain::DriverError>> DriverGateway::update(std::string driver_name,
                                                                           domain::DriverUpdateCommand command) {
    auto module = catalog_.find(driver_name);
    if (module == nullptr) {
        co_return std::unexpected(domain::DriverError{domain::DriverError::Code::NOT_FOUND,
                                                      fmt::format("Driver '{}' is not loaded", driver_name)});
    }
    const auto required = required_capability(command.rd_type);
    if (!required.has_value() || (module->descriptor().capabilities & *required) == 0) {
        co_return std::unexpected(unsupported_record(driver_name, command.rd_type, required));
    }
    auto state = std::make_shared<CallState>();
    const std::string fqdn = command.fqdn;
    auto owner = module->mode() == ExecutionMode::Async
                     ? make_async_update_owner(state, module, std::move(command), logger_, options_.http,
                                               options_.bridge_wait_budget)
                     : make_sync_update_owner(state, module, std::move(command), bridge_, logger_);
    auto& scope = co_await coro::current_scope();
    scope.throw_if_cancelled();
    const CycleSubscription subscription{*state, scope};
    start_cycle(cycle_group_, state, std::move(owner));
    co_await subscription.wait();
    co_return subscription.take_result(driver_name, fqdn);
}

bool DriverGateway::supports_validate(std::string_view driver_name) const {
    const auto module = catalog_.find(driver_name);
    return module != nullptr && module->supports_validate();
}

coro::Task<std::expected<void, domain::DriverError>> DriverGateway::validate_config(std::string driver_name,
                                                                                    std::string driver_param_json) {
    auto module = catalog_.find(driver_name);
    if (module == nullptr) {
        co_return std::unexpected(domain::DriverError{domain::DriverError::Code::NOT_FOUND,
                                                      fmt::format("Driver '{}' is not loaded", driver_name)});
    }
    if (!module->supports_validate()) {
        const auto entry =
            module->mode() == ExecutionMode::Async ? "yaddnsc_driver_async_validate" : "yaddnsc_driver_validate";
        co_return std::unexpected(domain::DriverError{
            domain::DriverError::Code::UNKNOWN,
            fmt::format("Driver '{}' does not provide {}; its configuration was not checked", driver_name, entry), 0});
    }
    auto state = std::make_shared<CallState>();
    auto owner = module->mode() == ExecutionMode::Async
                     ? make_async_validate_owner(state, module, std::move(driver_param_json), logger_)
                     : make_sync_validate_owner(state, module, std::move(driver_param_json), bridge_, logger_);
    auto& scope = co_await coro::current_scope();
    scope.throw_if_cancelled();
    const CycleSubscription subscription{*state, scope};
    start_cycle(cycle_group_, state, std::move(owner));
    co_await subscription.wait();
    co_return subscription.take_result(driver_name);
}

}  // namespace plugin
