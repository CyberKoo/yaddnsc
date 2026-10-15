/// Loader fixture for the ABI error-report contract at the host boundary.
///
/// Every mode violates the v1 C ABI contract in exactly one way, so the host's
/// reaction to that single violation can be attributed without interference
/// from the others. The gateway is expected to reject a contract-violating
/// report rather than trust it — the plugin is, by definition, outside the
/// host's control.
///
/// Contract rules exercised (SDK header, yaddnsc_error):
///   - on a non-OK status the callee must fill out_error when it is non-NULL
///     and large enough, and out_error.status must equal the returned status;
///   - on YADDNSC_STATUS_OK the callee must not write out_error, and create
///     must store the instance handle;
///   - an empty message (size 0) is a legal message, so a status carrying no
///     message is a legal report that the host still has to render.
///
/// abi_misbehave_set_mode() takes a mode *name* rather than an ordinal, so a
/// test and this fixture cannot drift apart by reordering an enum. It returns
/// 0 on success and -1 for an unknown name. abi_misbehave_reset_state() clears
/// the counters.
///
/// No mode reaches the host HTTP bridge, so tests need no server.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <yaddnsc/sdk/driver_abi.h>

#define FIXTURE_EXPORT __attribute__((visibility("default")))

namespace {

constexpr yaddnsc_driver_descriptor DESCRIPTOR = {
    .struct_size = sizeof(yaddnsc_driver_descriptor),
    .abi_major = YADDNSC_DRIVER_ABI_MAJOR,
    .abi_minor = YADDNSC_DRIVER_ABI_MINOR,
    .magic = YADDNSC_DRIVER_MAGIC,
    .name = {"abi_misbehave", sizeof("abi_misbehave") - 1},
    .version = {"0.0.0", sizeof("0.0.0") - 1},
    .author = {"yaddnsc", sizeof("yaddnsc") - 1},
    .description = {"Fixture that violates the error-report contract one way at a time",
                    sizeof("Fixture that violates the error-report contract one way at a time") - 1},
    .capabilities = YADDNSC_DRIVER_CAPABILITY_A,
};

constexpr char REPORTED_MESSAGE[] = "plugin reported this failure";
/// UINT32_MAX exercises the host's clamp instead of a narrowing conversion.
constexpr std::uint32_t OVERFLOWING_RETRY = UINT32_MAX;

/// A non-null instance pointer: the handle the contract requires create() to
/// store. Never dereferenced.
alignas(alignof(void*)) unsigned char g_instance[sizeof(void*)]{};

enum class Mode {
    kWellBehaved,
    kCreateOkWithoutHandle,       ///< OK, but *out_driver left untouched
    kCreateFailsWithoutReport,    ///< non-OK, out_error untouched
    kCreateReportsEmptyMessage,   ///< non-OK, legal report with an empty message
    kCreateFailsWithStaleStatus,  ///< out_error.status disagrees with the return value
    kCreateReportsInternalError,  ///< non-OK, legal report
    kUpdateFailsWithoutReport,    ///< non-OK, out_error untouched
    kUpdateReportsNetworkError,   ///< non-OK, legal report
    kUpdateReportsRateLimited,    ///< non-OK, legal report, overflowing retry_after
    kUpdateReportsInvalidArgument,///< a valid status outside the mapping's cases
    kValidateFailsWithoutReport,  ///< non-OK, out_error untouched
    kValidateRejectsConfig,       ///< non-OK INVALID_CONFIG, legal report
};

std::optional<Mode> parse_mode(const std::string_view name) {
    if (name == "well_behaved") {
        return Mode::kWellBehaved;
    }
    if (name == "create_ok_without_handle") {
        return Mode::kCreateOkWithoutHandle;
    }
    if (name == "create_fails_without_report") {
        return Mode::kCreateFailsWithoutReport;
    }
    if (name == "create_reports_empty_message") {
        return Mode::kCreateReportsEmptyMessage;
    }
    if (name == "create_fails_with_stale_status") {
        return Mode::kCreateFailsWithStaleStatus;
    }
    if (name == "create_reports_internal_error") {
        return Mode::kCreateReportsInternalError;
    }
    if (name == "update_fails_without_report") {
        return Mode::kUpdateFailsWithoutReport;
    }
    if (name == "update_reports_network_error") {
        return Mode::kUpdateReportsNetworkError;
    }
    if (name == "update_reports_rate_limited") {
        return Mode::kUpdateReportsRateLimited;
    }
    if (name == "update_reports_invalid_argument") {
        return Mode::kUpdateReportsInvalidArgument;
    }
    if (name == "validate_fails_without_report") {
        return Mode::kValidateFailsWithoutReport;
    }
    if (name == "validate_rejects_config") {
        return Mode::kValidateRejectsConfig;
    }
    return std::nullopt;
}

Mode g_mode = Mode::kWellBehaved;
std::uint64_t g_creates = 0;
std::uint64_t g_updates = 0;
std::uint64_t g_validates = 0;
std::uint64_t g_destroys = 0;

/// Fill out_error with a report consistent with `status`.
yaddnsc_status report(yaddnsc_error* out_error, yaddnsc_status status, std::string_view message) {
    if (out_error != nullptr && out_error->struct_size >= YADDNSC_ERROR_MIN_SIZE) {
        out_error->status = status;
        out_error->retry_after_seconds = 0;
        out_error->message = {message.data(), message.size()};
    }
    return status;
}
}  // namespace

extern "C" FIXTURE_EXPORT int abi_misbehave_set_mode(const char* name) {
    if (name == nullptr) {
        return -1;
    }
    // An unrecognised name is reported rather than silently falling back to the
    // well-behaved default, which would make a mistyped mode read as a pass.
    const auto mode = parse_mode(std::string_view{name});
    if (!mode.has_value()) {
        return -1;
    }
    g_mode = *mode;
    return 0;
}

extern "C" FIXTURE_EXPORT void abi_misbehave_reset_state(void) {
    g_creates = 0;
    g_updates = 0;
    g_validates = 0;
    g_destroys = 0;
}

extern "C" FIXTURE_EXPORT void abi_misbehave_get_state(std::uint64_t* creates, std::uint64_t* updates,
                                                       std::uint64_t* validates, std::uint64_t* destroys) {
    if (creates != nullptr) {
        *creates = g_creates;
    }
    if (updates != nullptr) {
        *updates = g_updates;
    }
    if (validates != nullptr) {
        *validates = g_validates;
    }
    if (destroys != nullptr) {
        *destroys = g_destroys;
    }
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_get_descriptor(const yaddnsc_driver_descriptor** out) {
    if (out == nullptr) {
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    *out = &DESCRIPTOR;
    return YADDNSC_STATUS_OK;
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_create(const yaddnsc_host_services* /*services*/,
                                                               yaddnsc_driver** out_driver, yaddnsc_error* out_error) {
    ++g_creates;
    switch (g_mode) {
        case Mode::kCreateOkWithoutHandle:
            // OK with no handle stored: the host must not dereference it.
            return YADDNSC_STATUS_OK;
        case Mode::kCreateFailsWithoutReport:
            // Non-OK while leaving out_error as the host initialised it.
            return YADDNSC_STATUS_INTERNAL_ERROR;
        case Mode::kCreateReportsEmptyMessage:
            // Legal report carrying no message: the host must synthesise one.
            return report(out_error, YADDNSC_STATUS_INTERNAL_ERROR, "");
        case Mode::kCreateFailsWithStaleStatus:
            // A report whose status disagrees with the returned status: the
            // host must not trust either half of it.
            report(out_error, YADDNSC_STATUS_NETWORK_ERROR, REPORTED_MESSAGE);
            return YADDNSC_STATUS_INTERNAL_ERROR;
        case Mode::kCreateReportsInternalError:
            return report(out_error, YADDNSC_STATUS_INTERNAL_ERROR, REPORTED_MESSAGE);
        default:
            break;
    }
    if (out_driver != nullptr) {
        *out_driver = reinterpret_cast<yaddnsc_driver*>(g_instance);
    }
    return YADDNSC_STATUS_OK;
}

extern "C" FIXTURE_EXPORT void yaddnsc_driver_destroy(yaddnsc_driver* /*driver*/) {
    ++g_destroys;
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_update(yaddnsc_driver* /*driver*/,
                                                               const yaddnsc_update_request* /*request*/,
                                                               yaddnsc_error* out_error) {
    ++g_updates;
    switch (g_mode) {
        case Mode::kUpdateFailsWithoutReport:
            return YADDNSC_STATUS_UPSTREAM_REJECTED;
        case Mode::kUpdateReportsNetworkError:
            return report(out_error, YADDNSC_STATUS_NETWORK_ERROR, REPORTED_MESSAGE);
        case Mode::kUpdateReportsRateLimited:
            if (out_error != nullptr && out_error->struct_size >= YADDNSC_ERROR_MIN_SIZE) {
                out_error->status = YADDNSC_STATUS_RATE_LIMITED;
                out_error->retry_after_seconds = OVERFLOWING_RETRY;
                out_error->message = {REPORTED_MESSAGE, sizeof(REPORTED_MESSAGE) - 1};
            }
            return YADDNSC_STATUS_RATE_LIMITED;
        case Mode::kUpdateReportsInvalidArgument:
            return report(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, REPORTED_MESSAGE);
        default:
            return YADDNSC_STATUS_OK;
    }
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_validate(yaddnsc_driver* /*driver*/,
                                                                 yaddnsc_string /*driver_param_json*/,
                                                                 yaddnsc_error* out_error) {
    ++g_validates;
    switch (g_mode) {
        case Mode::kValidateFailsWithoutReport:
            return YADDNSC_STATUS_INVALID_CONFIG;
        case Mode::kValidateRejectsConfig:
            return report(out_error, YADDNSC_STATUS_INVALID_CONFIG, REPORTED_MESSAGE);
        default:
            return YADDNSC_STATUS_OK;
    }
}
