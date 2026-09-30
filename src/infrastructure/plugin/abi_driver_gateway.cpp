//
// Created by Kotarou on 2026/9/17.
//

#include "abi_driver_gateway.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
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

AbiDriverGateway::AbiDriverGateway(const DriverCatalog& catalog, HttpClientFactory http_factory, const Logger& logger)
    : catalog_(catalog), http_factory_(std::move(http_factory)), logger_(logger) {}

namespace {
[[nodiscard]] domain::DriverError map_error(yaddnsc_status status, std::string_view plugin_message,
                                            std::string_view driver_name, std::string_view fqdn) {
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
    return value.data == nullptr ? std::string_view{} : std::string_view{value.data, value.size};
}

[[nodiscard]] bool has_valid_plugin_error(const yaddnsc_error& error, yaddnsc_status returned_status) noexcept {
    return yaddnsc_status_is_valid(returned_status) && error.struct_size >= YADDNSC_ERROR_MIN_SIZE &&
           yaddnsc_status_is_valid(error.status) && error.status == returned_status &&
           yaddnsc_string_is_valid(error.message);
}

/// Bit required for an ABI 1.0 record type. "A" and "AAAA" have bits;
/// every other type, including "TXT", returns nullopt.
[[nodiscard]] std::optional<uint64_t> required_capability(std::string_view record_type) noexcept {
    if (record_type == "A") {
        return YADDNSC_DRIVER_CAPABILITY_A;
    }
    if (record_type == "AAAA") {
        return YADDNSC_DRIVER_CAPABILITY_AAAA;
    }
    return std::nullopt;
}

/// Host-side record-type gate. Runs before create(), so the plugin is not
/// called and no yaddnsc_status is produced. The domain result is
/// UPDATE_FAILED, the same code map_error uses when a plugin returns
/// YADDNSC_STATUS_UNSUPPORTED_RECORD.
[[nodiscard]] domain::DriverError unsupported_record(std::string_view driver_name, std::string_view record_type,
                                                     std::optional<uint64_t> required) {
    const std::string message =
        required.has_value()
            ? fmt::format("Driver '{}' does not support record type {} (missing capability {})", driver_name,
                          record_type, *required == YADDNSC_DRIVER_CAPABILITY_AAAA ? "AAAA" : "A")
            : fmt::format("Driver '{}' does not support record type {} (no v1 capability)", driver_name, record_type);
    return {domain::DriverError::Code::UPDATE_FAILED, message, 0};
}

[[nodiscard]] domain::DriverError invalid_plugin_result(std::string_view driver_name, std::string_view entry) {
    return {domain::DriverError::Code::UNKNOWN,
            fmt::format("Driver '{}' returned an invalid {} ABI error report", driver_name, entry), 0};
}

/// yaddnsc_status → domain::DriverError for the validate path. Unlike the
/// update mapping there is no fqdn context; any non-OK status means the
/// configuration was rejected (INVALID_CONFIG is the canonical code, but
/// plugins may report other failures — e.g. an internal error while
/// validating — which the caller must surface verbatim).
[[nodiscard]] domain::DriverError map_validate_error(yaddnsc_status /*status*/, std::string_view plugin_message,
                                                     std::string_view driver_name) {
    const std::string message = !plugin_message.empty()
                                    ? std::string(plugin_message)
                                    : fmt::format("Driver '{}' rejected its driver_param configuration", driver_name);
    return {domain::DriverError::Code::UNKNOWN, message, 0};
}
}  // anonymous namespace

std::expected<void, domain::DriverError> AbiDriverGateway::update(std::string_view driver_name,
                                                                  const DriverUpdateCommand& command,
                                                                  const Utils::CancellationToken& token) const {
    auto module = catalog_.find(driver_name);
    if (module == nullptr) {
        return std::unexpected(domain::DriverError{domain::DriverError::Code::NOT_FOUND,
                                                   fmt::format("Driver '{}' is not loaded", driver_name)});
    }

    const auto required = required_capability(command.rd_type);
    if (!required.has_value() || (module->descriptor().capabilities & *required) == 0) {
        return std::unexpected(unsupported_record(driver_name, command.rd_type, required));
    }

    auto http_client = http_factory_();
    HostServicesContext context(*http_client, logger_, token);
    const auto services = context.make_services();

    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));

    yaddnsc_driver* handle = nullptr;
    const yaddnsc_status create_status = module->create(services, &handle, error);
    if (create_status == YADDNSC_STATUS_OK && handle == nullptr) {
        return std::unexpected(invalid_plugin_result(driver_name, "create"));
    }
    if (create_status != YADDNSC_STATUS_OK) {
        if (!has_valid_plugin_error(error, create_status)) {
            return std::unexpected(invalid_plugin_result(driver_name, "create"));
        }
        return std::unexpected(map_error(create_status, to_view(error.message), driver_name, command.fqdn));
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

    error = {};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    const yaddnsc_status status = instance.update(request, error);
    if (status == YADDNSC_STATUS_OK) {
        return {};
    }
    if (!has_valid_plugin_error(error, status)) {
        return std::unexpected(invalid_plugin_result(driver_name, "update"));
    }

    auto driver_error = map_error(status, to_view(error.message), driver_name, command.fqdn);
    // The ABI field is uint32; clamp instead of narrowing so an out-of-range
    // plugin value saturates at INT_MAX rather than going negative.
    driver_error.retry_after_seconds = static_cast<int>(std::min<std::uint32_t>(
        error.retry_after_seconds, static_cast<std::uint32_t>(std::numeric_limits<int>::max())));
    return std::unexpected(std::move(driver_error));
}

std::expected<void, domain::DriverError> AbiDriverGateway::validate_config(std::string_view driver_name,
                                                                           std::string_view driver_param_json) const {
    auto module = catalog_.find(driver_name);
    if (module == nullptr) {
        return std::unexpected(domain::DriverError{domain::DriverError::Code::NOT_FOUND,
                                                   fmt::format("Driver '{}' is not loaded", driver_name)});
    }

    // OPTIONAL entry (optional since ABI 1.0). The plugin still loads.
    // config test cannot confirm driver_param without the entry.
    if (!module->supports_validate()) {
        return std::unexpected(domain::DriverError{
            domain::DriverError::Code::UNKNOWN,
            fmt::format("Driver '{}' does not provide yaddnsc_driver_validate; its configuration was not checked",
                        driver_name),
            0});
    }

    auto http_client = http_factory_();
    HostServicesContext context(*http_client, logger_, {});
    const auto services = context.make_services(false);

    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));

    yaddnsc_driver* handle = nullptr;
    const yaddnsc_status create_status = module->create(services, &handle, error);
    if (create_status == YADDNSC_STATUS_OK && handle == nullptr) {
        return std::unexpected(invalid_plugin_result(driver_name, "create"));
    }
    if (create_status != YADDNSC_STATUS_OK) {
        if (!has_valid_plugin_error(error, create_status)) {
            return std::unexpected(invalid_plugin_result(driver_name, "create"));
        }
        return std::unexpected(map_validate_error(create_status, to_view(error.message), driver_name));
    }

    // The instance owns the destroy() call; copy error bytes out
    // synchronously after validate() returns, before destroy.
    DriverInstance instance(std::move(module), handle);

    error = {};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    const yaddnsc_string param{driver_param_json.data(), driver_param_json.size()};
    const yaddnsc_status status = instance.validate(param, error);
    if (status == YADDNSC_STATUS_OK) {
        return {};
    }
    if (!has_valid_plugin_error(error, status)) {
        return std::unexpected(invalid_plugin_result(driver_name, "validate"));
    }
    return std::unexpected(map_validate_error(status, to_view(error.message), driver_name));
}
