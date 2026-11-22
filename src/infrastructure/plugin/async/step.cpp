#include "step.h"

#include <string_view>
#include <utility>

#include "infrastructure/plugin/async/cleanup.h"
#include "infrastructure/plugin/detail/abi_result.h"

namespace plugin::detail {

AsyncStepOutcome make_step_outcome(const yaddnsc_status status, const yaddnsc_error& error) {
    AsyncStepOutcome outcome;
    outcome.status = status;
    if (status == YADDNSC_STATUS_OK || status == YADDNSC_STATUS_PENDING) {
        return outcome;
    }
    if (has_valid_plugin_error(error, status)) {
        outcome.message.assign(error.message.data, error.message.size);
        outcome.retry_after_seconds = error.retry_after_seconds;
        outcome.error_valid = true;
    }
    return outcome;
}

AsyncStepOutcome enforce_destroy_step_result(coro::ScopeOutcome<AsyncStepOutcome> outcome,
                                             const std::string_view driver, const std::string_view phase) noexcept {
    if (!outcome.has_value()) {
        fatal_async_cleanup_failure(driver, phase);
    }
    return std::move(*outcome);
}

}  // namespace plugin::detail
