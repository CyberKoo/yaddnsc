#include "driver_gateway.h"

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <yaddnsc/util/format.hpp>

#include "coro/scope.hpp"
#include "domain/dns/record_kind.h"
#include "domain/error/error.h"
#include "domain/update/driver_update_command.h"
#include "infrastructure/plugin/detail/cycle_runner.h"
#include "infrastructure/plugin/detail/session_dependencies.h"
#include "infrastructure/plugin/driver_catalog.h"
#include "infrastructure/plugin/plugin_package.h"
#include "support/fmt.hpp"

namespace plugin {
namespace {

[[nodiscard]] domain::DriverError unsupported_record(std::string_view driver_name,
                                                     const domain::RecordKind record_kind) {
    const auto text = domain::record_kind_to_str(record_kind);
    const auto message =
        record_kind == domain::RecordKind::A || record_kind == domain::RecordKind::AAAA
            ? fmt::format("Driver '{}' does not support record type {} (missing capability {})", driver_name, text,
                          text)
            : fmt::format("Driver '{}' does not support record type {} (no v1 capability)", driver_name, text);
    return {domain::DriverError::Code::UPDATE_FAILED, message, 0};
}

}  // namespace

DriverGateway::DriverGateway(const DriverCatalog& catalog, const app::LoggerPort& logger, coro::Loop& loop,
                             coro::Nursery& cycle_nursery)
    : DriverGateway(catalog, logger, loop, cycle_nursery, Options{}) {}

DriverGateway::DriverGateway(const DriverCatalog& catalog, const app::LoggerPort& logger, coro::Loop& loop,
                             coro::Nursery& cycle_nursery, Options options)
    : catalog_(catalog), logger_(logger), loop_(loop), cycle_nursery_(cycle_nursery),
      cycle_nursery_token_(coro::nursery_token(cycle_nursery)), options_(std::move(options)) {}

SessionDependencies DriverGateway::dependencies() const {
    return {logger_, loop_, cycle_nursery_, options_.http, options_.bridge_wait_budget};
}

coro::Task<std::expected<void, domain::DriverError>> DriverGateway::update(std::string driver_name,
                                                                           domain::DriverUpdateCommand command) {
    auto package = catalog_.find(driver_name);
    if (package == nullptr) {
        co_return std::unexpected(domain::DriverError{domain::DriverError::Code::NOT_FOUND,
                                                      fmt::format("Driver '{}' is not loaded", driver_name)});
    }
    if (!package->capabilities().supports(command.record_kind)) {
        co_return std::unexpected(unsupported_record(driver_name, command.record_kind));
    }

    const std::string fqdn = command.fqdn;
    auto session = package->create_update_session(std::move(command), dependencies());
    auto& scope = co_await coro::current_scope();
    plugin::CycleRunner runner{scope, std::move(session), cycle_nursery_token_, driver_name, fqdn};
    co_await runner.result_ready().wait();
    co_return runner.take_result();
}

bool DriverGateway::supports_validate(const std::string_view driver_name) const {
    const auto package = catalog_.find(driver_name);
    return package != nullptr && package->supports_validate();
}

coro::Task<std::expected<void, domain::DriverError>> DriverGateway::validate_config(std::string driver_name,
                                                                                    std::string driver_param_json) {
    auto package = catalog_.find(driver_name);
    if (package == nullptr) {
        co_return std::unexpected(domain::DriverError{domain::DriverError::Code::NOT_FOUND,
                                                      fmt::format("Driver '{}' is not loaded", driver_name)});
    }
    if (!package->supports_validate()) {
        co_return std::unexpected(
            domain::DriverError{domain::DriverError::Code::UNKNOWN,
                                fmt::format("Driver '{}' does not provide {}; its configuration was not checked",
                                            driver_name, package->validation_entry_name()),
                                0});
    }

    auto session = package->create_validation_session(std::move(driver_param_json), dependencies());
    auto& scope = co_await coro::current_scope();
    plugin::CycleRunner runner{scope, std::move(session), cycle_nursery_token_, driver_name};
    co_await runner.result_ready().wait();
    co_return runner.take_result();
}

}  // namespace plugin
