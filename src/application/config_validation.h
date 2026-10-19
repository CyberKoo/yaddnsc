#ifndef YADDNSC_APPLICATION_CONFIG_VALIDATION_H
#define YADDNSC_APPLICATION_CONFIG_VALIDATION_H

#include <string>
#include <vector>

#include "coro/task.hpp"
#include "coro/time.h"

namespace domain {
struct RuntimeConfig;
}

namespace app {

class GatewayPort;

/// How a driver without the optional validation entry is treated.
enum class ConfigValidationMode {
    /// The entry is required — "cannot confirm" is not "valid". This is
    /// `config test`, whose entire purpose is to answer whether the
    /// configuration is good.
    REQUIRE_VALIDATION,
    /// The entry is optional: such a driver is named in `unchecked_drivers`
    /// and skipped. This is `run`, so a plugin built against an older ABI
    /// cannot stop a daemon that would otherwise work.
    WARN_AND_CONTINUE,
};

/// Outcome of a configuration-validation sweep.
struct ConfigValidationOutcome {
    /// Drivers whose parameters were not checked, each listed once. Under
    /// WARN_AND_CONTINUE these are warnings; under REQUIRE_VALIDATION the
    /// sweep stops at the first one and puts it in `rejection`.
    std::vector<std::string> unchecked_drivers;
    /// Empty when the sweep passed; otherwise the driver/FQDN diagnostic that
    /// failed it (an explicit rejection, an expired budget, or — under
    /// REQUIRE_VALIDATION — a driver with no validation entry).
    std::string rejection;
};

/// Check each subdomain's driver parameters through the gateway, stopping at
/// the first rejection. Each record gets its own `budget`, so one slow plugin
/// cannot consume the whole sweep; a record that runs out stops the sweep and
/// is reported as a rejection. Records sharing an identical (driver,
/// driver_params) pair are validated once — a duplicate reuses the first
/// verdict instead of another plugin round-trip. Cancellation (an ancestor
/// scope, including the shutdown the signal watchers request) aborts the sweep
/// and propagates; a validation call already inside a plugin is synchronous
/// and runs to completion. Defects propagate. The borrowed gateway/config must
/// outlive this loop-thread task.
[[nodiscard]] coro::Task<ConfigValidationOutcome> validate_driver_configs(GatewayPort& gateway,
                                                                          const domain::RuntimeConfig& config,
                                                                          ConfigValidationMode mode,
                                                                          coro::Duration budget);

}  // namespace app

#endif  // YADDNSC_APPLICATION_CONFIG_VALIDATION_H
