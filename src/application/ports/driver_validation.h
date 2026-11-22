#ifndef YADDNSC_APPLICATION_PORTS_DRIVER_VALIDATION_H
#define YADDNSC_APPLICATION_PORTS_DRIVER_VALIDATION_H

#include <expected>
#include <string>
#include <string_view>

#include "coro/task.hpp"
#include "domain/error/error.h"

namespace app {

/// DriverValidationPort — the application port that checks a driver's
/// parameters without publishing anything.
///
/// Failure: DriverError values. Cancellation propagates as `coro::Cancelled`:
/// the await is an abandon point. The port must outlive the task.
/// Thread safety: loop-thread only.
class DriverValidationPort {
public:
    virtual ~DriverValidationPort() = default;

    /// Whether this driver can have its parameters checked through
    /// validate_config. False when the plugin does not export the optional
    /// validation entry: the driver still loads and updates, but the host
    /// cannot confirm its configuration.
    [[nodiscard]] virtual bool supports_validate(std::string_view driver_name) const = 0;

    /// Diagnostic path for `config test`, outside the update-loop lifecycle.
    /// Validate driver parameters without updating. A missing validation entry
    /// returns DriverError; cancellation propagates.
    [[nodiscard]] virtual coro::Task<std::expected<void, domain::DriverError>> validate_config(
        std::string driver_name, std::string driver_param_json) = 0;
};

}  // namespace app

#endif  // YADDNSC_APPLICATION_PORTS_DRIVER_VALIDATION_H
