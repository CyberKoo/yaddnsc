//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DRIVER_INTERFACE_H
#define YADDNSC_DRIVER_INTERFACE_H

#include <string>
#include <string_view>

#include "http_type.h"
#include "abi_version.h"
#include "http_client.h"

using DriverConfig = std::string;
using DriverParams = HttpParams;
using DriverHttpMethod = HttpMethod;
using DriverRequest = HttpRequest;

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

class Driver {
public:
    // Default construct
    Driver() = default;

    // deconstruct
    virtual ~Driver() = default;

    // delete copy and move constructors and assign operators
    // Copy construct
    Driver(Driver const &) = delete;

    // Move construct
    Driver(Driver &&) = delete;

    // Copy assign
    Driver &operator=(Driver const &) = delete;

    // Move assign
    Driver &operator=(Driver &&) = delete;

public:
    [[nodiscard]] virtual DriverRequest generate_request(
        const DriverConfig &config, const UpdateContext &ctx
    ) const = 0;

    [[nodiscard]] virtual bool check_response(const HttpResponse &response) const = 0;

    [[nodiscard]] virtual DriverDetail get_detail() const = 0;

    [[nodiscard]] virtual AbiVersion get_abi_version() const = 0;

    [[nodiscard]] virtual bool execute(
        const DriverConfig &config, const UpdateContext &ctx, HttpClient &http) const = 0;
};

#endif //YADDNSC_DRIVER_INTERFACE_H
