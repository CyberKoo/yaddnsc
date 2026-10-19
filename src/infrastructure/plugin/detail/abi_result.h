#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_DETAIL_ABI_RESULT_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_DETAIL_ABI_RESULT_H

#include <string_view>

#include <yaddnsc/sdk/driver_abi.h>

#include "domain/error/error.h"

namespace plugin::detail {

/// Worker-to-loop cancellation marker; never crosses the plugin C ABI.
struct AbiCancelled {};

/// Error reports must match the returned completion status and cover all views.
[[nodiscard]] bool has_valid_plugin_error(const yaddnsc_error& error, yaddnsc_status returned_status) noexcept;

/// Translate an update/validation failure. CANCELLED throws AbiCancelled;
/// allocation failures propagate. The caller copies borrowed plugin text before
/// the next ABI entry. These helpers perform no logging or recovery.
[[nodiscard]] domain::DriverError map_error(yaddnsc_status status, std::string_view plugin_message,
                                            std::string_view driver_name, std::string_view fqdn);
[[nodiscard]] domain::DriverError map_validate_error(yaddnsc_status status, std::string_view plugin_message,
                                                     std::string_view driver_name);
[[nodiscard]] domain::DriverError invalid_plugin_result(std::string_view driver_name, std::string_view entry);

}  // namespace plugin::detail

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_DETAIL_ABI_RESULT_H
