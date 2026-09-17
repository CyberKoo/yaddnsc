//
// Created by Kotarou on 2026/9/17.
//

#include "cpp_driver_gateway.h"

#include <exception>
#include <utility>

#include "driver_manager.h"
#include "interface/driver.h"
#include "exception/driver_not_found.h"

#include "fmt.hpp"

CppDriverGateway::CppDriverGateway(const DriverManagerBase &driver_manager, HttpClientFactory http_factory)
    : driver_manager_(driver_manager), http_factory_(std::move(http_factory)) {
}

std::expected<void, domain::DriverError>
CppDriverGateway::update(std::string_view driver_name, const DriverUpdateCommand &command) const {
    const Driver *driver = nullptr;
    try {
        driver = &driver_manager_.get_driver(std::string(driver_name));
    } catch (const DriverNotFoundException &e) {
        return std::unexpected(domain::DriverError{domain::DriverError::Code::NOT_FOUND, e.what()});
    }

    const DriverUpdateParams params{
        .ip_addr = command.ip_addr,
        .rd_type = command.rd_type,
        .domain = command.domain,
        .subdomain = command.subdomain,
        .fqdn = command.fqdn,
    };

    try {
        auto http_client = http_factory_();
        if (!driver->execute(command.driver_param, params, *http_client)) {
            // The driver already logged the HTTP/upstream failure itself.
            return std::unexpected(domain::DriverError{
                domain::DriverError::Code::UPDATE_FAILED,
                fmt::format("Driver '{}' update failed for {}", driver_name, command.fqdn)});
        }
    } catch (const std::exception &e) {
        return std::unexpected(domain::DriverError{domain::DriverError::Code::UNKNOWN, e.what()});
    } catch (...) {
        return std::unexpected(
            domain::DriverError{domain::DriverError::Code::UNKNOWN, "unknown non-standard exception"});
    }

    return {};
}
