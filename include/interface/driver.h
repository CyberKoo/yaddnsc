//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DRIVER_INTERFACE_H
#define YADDNSC_DRIVER_INTERFACE_H

#include <string>
#include <string_view>

#include "abi_version.h"
#include "http_client.h"
#include "http_type.h"

using DriverConfig = std::string;
using DriverParams = HttpParams;
using DriverHttpMethod = HttpMethod;
using DriverRequest = HttpRequest;

// ---------------------------------------------------------------------------
//  Driver detail — static metadata exposed by each driver
// ---------------------------------------------------------------------------

struct DriverDetail final {
    const std::string_view name;
    const std::string_view description;
    const std::string_view author;
    const std::string_view version;
};

// ---------------------------------------------------------------------------
//  DriverRequestContext — the result of a driver's request generation
// ---------------------------------------------------------------------------

// Combined result from generate_request — the URL is passed separately
// to HttpClient::exchange(), not embedded in HttpRequest.
struct DriverRequestContext {
    std::string url;
    HttpRequest request;
};

// ---------------------------------------------------------------------------
//  DriverUpdateParams — per-update parameters consumed by drivers
// ---------------------------------------------------------------------------

// Immutable after construction; create via aggregate / designated init only.
// Populated by the updater from the current task + resolved IP address.
struct DriverUpdateParams final {
    const std::string ip_addr;
    const std::string rd_type;
    const std::string domain;
    const std::string subdomain;
    const std::string fqdn;
};

// ---------------------------------------------------------------------------
//  Driver interface — every DNS backend must implement this
// ---------------------------------------------------------------------------

class Driver {
public:
    Driver() = default;
    virtual ~Driver() = default;

    Driver(const Driver &) = delete;
    Driver &operator=(const Driver &) = delete;
    Driver(Driver &&) = delete;
    Driver &operator=(Driver &&) = delete;

    [[nodiscard]] virtual DriverRequestContext generate_request(
        const DriverConfig &config, const DriverUpdateParams &ctx
    ) const = 0;

    [[nodiscard]] virtual bool check_response(const HttpResponse &response) const = 0;

    [[nodiscard]] virtual DriverDetail get_detail() const = 0;

    [[nodiscard]] virtual AbiVersion get_abi_version() const = 0;

    [[nodiscard]] virtual bool execute(
        const DriverConfig &config, const DriverUpdateParams &ctx, HttpClient &http
    ) const = 0;
};

#endif // YADDNSC_DRIVER_INTERFACE_H
