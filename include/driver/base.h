//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DRIVER_BASE_H
#define YADDNSC_DRIVER_BASE_H

#include <glaze/glaze.hpp>

#include "interface/driver.h"
#include "interface/core_logger.h"
#include "interface/http_client.h"

#include "driver_ver.h"
#include "http_fmt.hpp"
#include "exceptions.h"

/// Convenience base class for yaddnsc driver plugins.
///
/// Provides:
///   - A default `execute()` implementation that follows the standard
///     generate-request → HTTP exchange → check-response pipeline.
///   - `parse_config<T>()` for type-safe JSON config deserialisation with
///     built-in error reporting.
///   - Automatic ABI version reporting via `get_abi_version()`.
///
/// Driver plugins should inherit from this class and override:
///   - `generate_request()`  — build the API request from config + update params
///   - `check_response()`    — validate the upstream API response
///   - `get_detail()`        — return static metadata about the driver
class BaseDriver : public Driver {
public:
    /// Return the ABI version compiled into this driver plugin.
    [[nodiscard]] AbiVersion get_abi_version() const final {
        return DRV_ABI_VERSION;
    }

    /// Default execute: generate_request -> exchange via HttpClient -> check_response.
    ///
    /// Logs each step via CORE_LOG_* macros. Returns false on HTTP error
    /// or upstream rejection rather than throwing.
    bool execute(const DriverConfig &config, const DriverUpdateParams &ctx, HttpClient &http) const override {
        const auto [url, request] = generate_request(config, ctx);
        CORE_LOG_DEBUG("Domain {} ({}) received DNS record update request from driver {}, {}", ctx.fqdn, ctx.rd_type,
                       get_detail().name, request);

        const auto response = http.exchange(url, request);
        if (!response) {
            CORE_LOG_WARN("Domain {} ({}) update failed (HTTP error: {})", ctx.fqdn, ctx.rd_type, response.error());
            return false;
        }

        if (!check_response(*response)) {
            CORE_LOG_WARN("Domain {} ({}) update rejected by upstream", ctx.fqdn, ctx.rd_type);
            return false;
        }

        return true;
    }

protected:
    /// Parse driver config JSON into a typed struct with built-in validation.
    ///
    /// Requires `glz::meta` specialisation for `T`. On failure, logs the
    /// glaze error and throws ParamParseException.
    ///
    /// @tparam T  Target struct type with a glz::meta specialisation.
    /// @param config  Raw JSON string from the application config.
    /// @return        Deserialised config struct.
    /// @throws ParamParseException  When required keys are missing or values
    ///                              are malformed.
    template<typename T>
    [[nodiscard]] static T parse_config(const DriverConfig &config) {
        T value{};
        const auto ec = glz::read<glz::opts{.error_on_missing_keys = true}>(value, config, glz::context{});
        if (ec == glz::error_code::none) [[likely]] {
            return value;
        }

        throw ParamParseException(glz::format_error(ec, config));
    }
};

extern "C" Driver *create();

#endif //YADDNSC_DRIVER_BASE_H
