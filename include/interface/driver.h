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

/// Opaque driver configuration string (raw JSON).
using DriverConfig = std::string;

/// HTTP parameter map used in driver requests.
using DriverParams = HttpParams;

/// HTTP method alias for driver use.
using DriverHttpMethod = HttpMethod;

/// HTTP request alias for driver use.
using DriverRequest = HttpRequest;

/// Static metadata exposed by each driver.
struct DriverDetail final {
    const std::string_view name;        ///< Driver display name, e.g. "cloudflare"
    const std::string_view description; ///< Human-readable description of what this driver does
    const std::string_view author;      ///< Driver author name / handle
    const std::string_view version;     ///< Driver version (semver string)
};

/// Combined result from a driver's request generation.
///
/// The URL is passed separately to HttpClient::exchange(), not embedded
/// in HttpRequest.
struct DriverRequestContext {
    std::string url;     ///< Target URL for the API call
    HttpRequest request; ///< HTTP request body, headers, and method
};

/// Per-update parameters consumed by drivers.
///
/// Immutable after construction; created via aggregate/designated init only.
/// Populated by the updater from the current task + resolved IP address.
struct DriverUpdateParams final {
    const std::string ip_addr;   ///< Resolved IP address to update
    const std::string rd_type;   ///< DNS record type as string (e.g. "A", "AAAA")
    const std::string domain;    ///< Parent domain name
    const std::string subdomain; ///< Subdomain label (may be "@" or empty for apex)
    const std::string fqdn;      ///< Fully qualified domain name (subdomain.domain)
};

/// Driver interface — every DNS backend must implement this.
///
/// A driver encapsulates the logic to:
///   1. Build an API request from config and update params (`generate_request`).
///   2. Validate the upstream response (`check_response`).
///   3. (Optionally) orchestrate the full HTTP flow (`execute`).
///
/// Implementations should inherit from BaseDriver which provides a default
/// `execute()` and `get_abi_version()`.
class Driver {
public:
    Driver() = default;

    virtual ~Driver() = default;

    Driver(const Driver &) = delete;

    Driver &operator=(const Driver &) = delete;

    Driver(Driver &&) = delete;

    Driver &operator=(Driver &&) = delete;

    /// Build the API request for a DNS record update.
    /// @param config  Driver-specific JSON configuration string.
    /// @param ctx     Per-update parameters (IP, domain, etc.).
    /// @return        URL + HttpRequest ready to be sent via HttpClient.
    [[nodiscard]] virtual DriverRequestContext generate_request(
        const DriverConfig &config, const DriverUpdateParams &ctx
    ) const = 0;

    /// Validate the upstream API response.
    /// @param response  The HTTP response received from the API server.
    /// @return true if the update was accepted by the upstream service.
    [[nodiscard]] virtual bool check_response(const HttpResponse &response) const = 0;

    /// Return static metadata about this driver.
    /// @note Cannot throw — returns only compile-time-known data.
    [[nodiscard]] virtual DriverDetail get_detail() const noexcept = 0;

    /// Return the ABI version of this driver plugin.
    /// @note Cannot throw — returns only compile-time-known data.
    [[nodiscard]] virtual AbiVersion get_abi_version() const noexcept = 0;

    /// Execute a full update cycle: generate request → exchange → check response.
    ///
    /// The default implementation in BaseDriver is sufficient for most drivers.
    /// Override only if the driver requires custom HTTP handling (e.g. multiple
    /// round trips, session tokens, etc.).
    [[nodiscard]] virtual bool execute(
        const DriverConfig &config, const DriverUpdateParams &ctx, HttpClient &http
    ) const = 0;
};

#endif // YADDNSC_DRIVER_INTERFACE_H
