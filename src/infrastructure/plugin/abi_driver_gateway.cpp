//
// Created by Kotarou on 2026/9/17.
//

#include "abi_driver_gateway.h"

#include <string>
#include <utility>

#include <stdint.h>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/util/format.hpp>

#include "infrastructure/network/http/client_port.h"  // IWYU pragma: keep — HttpClient must be complete here
#include "infrastructure/plugin/plugin_loader.h"
#include "support/fmt.hpp"

#include "driver_catalog.h"
#include "driver_instance.h"
#include "host_services.h"

AbiDriverGateway::AbiDriverGateway(const DriverCatalog& catalog,
                                   HttpClientFactory http_factory,
                                   Utils::CancellationToken http_token,
                                   const Logger& logger)
    : catalog_(catalog), http_factory_(std::move(http_factory)), http_token_(std::move(http_token)), logger_(logger) {}

namespace {
[[nodiscard]] domain::DriverError map_error(yaddnsc_status status,
                                            std::string_view plugin_message,
                                            std::string_view driver_name,
                                            std::string_view fqdn) {
    using Code = domain::DriverError::Code;
    const std::string message = !plugin_message.empty()
                                    ? std::string(plugin_message)
                                    : fmt::format("Driver '{}' update failed for {}", driver_name, fqdn);
    switch (status) {
        case YADDNSC_STATUS_RATE_LIMITED:
            return {Code::RATE_LIMITED, message, 0};
        case YADDNSC_STATUS_CANCELLED:
            return {Code::CANCELLED, message, 0};
        case YADDNSC_STATUS_INVALID_CONFIG:
        case YADDNSC_STATUS_INTERNAL_ERROR:
            return {Code::UNKNOWN, message, 0};
        case YADDNSC_STATUS_NETWORK_ERROR:
        case YADDNSC_STATUS_AUTHENTICATION_FAILED:
        case YADDNSC_STATUS_UPSTREAM_REJECTED:
        case YADDNSC_STATUS_UNSUPPORTED_RECORD:
        case YADDNSC_STATUS_INVALID_RESPONSE:
            return {Code::UPDATE_FAILED, message, 0};
        default:
            // INVALID_ARGUMENT and unknown codes indicate a contract bug.
            return {Code::UNKNOWN, message, 0};
    }
}

[[nodiscard]] std::string_view to_view(yaddnsc_string value) noexcept {
    return {value.data, value.size};
}

/// yaddnsc_status → domain::DriverError for the validate path. Unlike the
/// update mapping there is no fqdn context; any non-OK status means the
/// configuration was rejected (INVALID_CONFIG is the canonical code, but
/// plugins may report other failures — e.g. an internal error while
/// validating — which the caller must surface verbatim).
[[nodiscard]] domain::DriverError map_validate_error(yaddnsc_status /*status*/,
                                                     std::string_view plugin_message,
                                                     std::string_view driver_name) {
    const std::string message = !plugin_message.empty()
                                    ? std::string(plugin_message)
                                    : fmt::format("Driver '{}' rejected its driver_param configuration", driver_name);
    return {domain::DriverError::Code::UNKNOWN, message, 0};
}
}  // anonymous namespace

std::expected<void, domain::DriverError> AbiDriverGateway::update(std::string_view driver_name,
                                                                  const DriverUpdateCommand& command) const {
    auto module = catalog_.find(driver_name);
    if (module == nullptr) {
        return std::unexpected(domain::DriverError{domain::DriverError::Code::NOT_FOUND,
                                                   fmt::format("Driver '{}' is not loaded", driver_name)});
    }

    auto http_client = http_factory_();
    HostServicesContext context(*http_client, logger_, http_token_);
    const auto services = context.make_services();

    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));

    yaddnsc_driver* handle = nullptr;
    if (const yaddnsc_status status = module->create(services, &handle, error); status != YADDNSC_STATUS_OK) {
        return std::unexpected(map_error(status, to_view(error.message), driver_name, command.fqdn));
    }

    // From here on the instance owns the destroy() call; copy error bytes
    // out synchronously after update() returns, before destroy.
    DriverInstance instance(std::move(module), handle);

    const yaddnsc_update_request request{
        .struct_size = static_cast<uint32_t>(sizeof(request)),
        .ip_address = {command.ip_addr.data(), command.ip_addr.size()},
        .record_type = {command.rd_type.data(), command.rd_type.size()},
        .domain = {command.domain.data(), command.domain.size()},
        .subdomain = {command.subdomain.data(), command.subdomain.size()},
        .fqdn = {command.fqdn.data(), command.fqdn.size()},
        .driver_param_json = {reinterpret_cast<const uint8_t*>(command.driver_param.data()),
                              command.driver_param.size()},
    };

    const yaddnsc_status status = instance.update(request, error);
    if (status == YADDNSC_STATUS_OK) {
        return {};
    }

    auto driver_error = map_error(status, to_view(error.message), driver_name, command.fqdn);
    if (status == YADDNSC_STATUS_RATE_LIMITED) {
        driver_error.retry_after_seconds = static_cast<int>(error.retry_after_seconds);
    }
    return std::unexpected(std::move(driver_error));
}

std::expected<void, domain::DriverError> AbiDriverGateway::validate_config(std::string_view driver_name,
                                                                           std::string_view driver_param_json) const {
    auto module = catalog_.find(driver_name);
    if (module == nullptr) {
        return std::unexpected(domain::DriverError{domain::DriverError::Code::NOT_FOUND,
                                                   fmt::format("Driver '{}' is not loaded", driver_name)});
    }

    // OPTIONAL entry: plugins built against an SDK without it are skipped.
    if (!module->supports_validate()) {
        return {};
    }

    auto http_client = http_factory_();
    HostServicesContext context(*http_client, logger_, {});
    const auto services = context.make_services();

    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));

    yaddnsc_driver* handle = nullptr;
    if (const yaddnsc_status status = module->create(services, &handle, error); status != YADDNSC_STATUS_OK) {
        return std::unexpected(map_validate_error(status, to_view(error.message), driver_name));
    }

    // The instance owns the destroy() call; copy error bytes out
    // synchronously after validate() returns, before destroy.
    DriverInstance instance(std::move(module), handle);

    const yaddnsc_string param{driver_param_json.data(), driver_param_json.size()};
    const yaddnsc_status status = instance.validate(param, error);
    if (status == YADDNSC_STATUS_OK) {
        return {};
    }
    return std::unexpected(map_validate_error(status, to_view(error.message), driver_name));
}
