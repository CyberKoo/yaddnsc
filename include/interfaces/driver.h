//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DRIVER_INTERFACE_H
#define YADDNSC_DRIVER_INTERFACE_H

#include <string>
#include <string_view>

#include "http_types.h"
#include "http_client.h"

using driver_config_type = std::string;
using driver_param_type = http_param_type;
using driver_http_method_type = http_method_type;
using driver_request = http_request;

struct DriverDetail final {
    const std::string_view name;
    const std::string_view description;
    const std::string_view author;
    const std::string_view version;
};

// Runtime context injected by the updater alongside the static config.
struct UpdateContext final {
    std::string ip_addr;
    std::string rd_type;
    std::string domain;
    std::string subdomain;
    std::string fqdn;
};

class IDriver {
public:
    // Default construct
    IDriver() = default;

    // deconstruct
    virtual ~IDriver() = default;

    // delete copy and move constructors and assign operators
    // Copy construct
    IDriver(IDriver const &) = delete;

    // Move construct
    IDriver(IDriver &&) = delete;

    // Copy assign
    IDriver &operator=(IDriver const &) = delete;

    // Move assign
    IDriver &operator=(IDriver &&) = delete;

public:
    [[nodiscard]] virtual driver_request generate_request(
        const driver_config_type &, const UpdateContext &) const = 0;

    [[nodiscard]] virtual bool check_response(std::string_view) const = 0;

    [[nodiscard]] virtual DriverDetail get_detail() const = 0;

    [[nodiscard]] virtual uint32_t get_driver_version() const = 0;

    [[nodiscard]] virtual bool execute(const driver_config_type &, const UpdateContext &, IHttpSender &) = 0;
};

#endif //YADDNSC_DRIVER_INTERFACE_H
