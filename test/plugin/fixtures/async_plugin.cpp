/// Pure C ABI async fixture. No SDK, no sync entry points.
///
/// It exports get_descriptor plus the complete async group and implements a
/// one-request state machine: begin() starts one HTTP request and returns
/// PENDING; resume() completes on HTTP_FINISHED or UPDATE_CANCELLED. The test
/// host supplies the async services table, so this file exercises only the C
/// ABI contract, not the production host or the C++ SDK.

#include <cstdint>
#include <new>
#include <string_view>

#include <yaddnsc/sdk/driver_abi.h>

#define FIXTURE_EXPORT __attribute__((visibility("default")))

namespace {

constexpr yaddnsc_driver_descriptor DESCRIPTOR = {
    .struct_size = sizeof(yaddnsc_driver_descriptor),
    .abi_major = YADDNSC_DRIVER_ABI_MAJOR,
    .abi_minor = YADDNSC_DRIVER_ABI_MINOR,
    .magic = YADDNSC_DRIVER_MAGIC,
    .name = {"async_fixture", sizeof("async_fixture") - 1},
    .version = {"1.0.0", sizeof("1.0.0") - 1},
    .author = {"yaddnsc", sizeof("yaddnsc") - 1},
    .description = {"Pure C ABI async fixture", sizeof("Pure C ABI async fixture") - 1},
    .capabilities = YADDNSC_DRIVER_CAPABILITY_A,
};

constexpr yaddnsc_string BAD_ARGUMENT{"invalid async argument", sizeof("invalid async argument") - 1};
constexpr yaddnsc_string CANCELLED_MESSAGE{"update cancelled", sizeof("update cancelled") - 1};
constexpr yaddnsc_string EVENT_ERROR_MESSAGE{"async fixture event error", sizeof("async fixture event error") - 1};
constexpr yaddnsc_string ZERO_OP_MESSAGE{"http_start returned a zero op id", sizeof("http_start returned a zero op id") - 1};
constexpr yaddnsc_string EMPTY_CONFIG_MESSAGE{"empty driver_params", sizeof("empty driver_params") - 1};

void write_error(yaddnsc_error* out_error, yaddnsc_status status, yaddnsc_string message) noexcept {
    if (out_error == nullptr || out_error->struct_size < YADDNSC_ERROR_MIN_SIZE) {
        return;
    }
    out_error->status = status;
    out_error->retry_after_seconds = 0;
    out_error->message = message;
    out_error->struct_size = out_error->struct_size < static_cast<uint32_t>(sizeof(yaddnsc_error))
                                 ? out_error->struct_size
                                 : static_cast<uint32_t>(sizeof(yaddnsc_error));
}

[[nodiscard]] bool valid_services(const yaddnsc_async_services* services) noexcept {
    return services != nullptr && services->struct_size >= YADDNSC_ASYNC_SERVICES_MIN_SIZE &&
           services->abi_major == YADDNSC_DRIVER_ABI_MAJOR && services->abi_minor >= 1 &&
           services->context != nullptr && services->log != nullptr && services->is_cancelled != nullptr &&
           services->http_start != nullptr && services->http_cancel != nullptr;
}

[[nodiscard]] bool valid_request(const yaddnsc_update_request* request) noexcept {
    if (request == nullptr || request->struct_size < YADDNSC_UPDATE_REQUEST_MIN_SIZE) {
        return false;
    }
    return yaddnsc_string_is_valid(request->ip_address) && yaddnsc_string_is_valid(request->record_type) &&
           yaddnsc_string_is_valid(request->domain) && yaddnsc_string_is_valid(request->subdomain) &&
           yaddnsc_string_is_valid(request->fqdn) && yaddnsc_bytes_is_valid(request->driver_param_json);
}

[[nodiscard]] bool valid_response(const yaddnsc_http_response* response) noexcept {
    if (response == nullptr || response->struct_size < YADDNSC_HTTP_RESPONSE_MIN_SIZE ||
        !yaddnsc_bytes_is_valid(response->body) ||
        !yaddnsc_http_header_array_is_valid(response->headers, response->header_count)) {
        return false;
    }
    for (size_t i = 0; i < response->header_count; ++i) {
        if (!yaddnsc_http_header_is_valid(response->headers[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool valid_error(const yaddnsc_error* error, yaddnsc_status status) noexcept {
    return error != nullptr && error->struct_size >= YADDNSC_ERROR_MIN_SIZE && error->status == status &&
           yaddnsc_string_is_valid(error->message);
}

enum class Phase {
    Initial,
    UpdateWaiting,
    UpdateTerminal,
    ValidateTerminal,
};

struct AsyncState {
    const yaddnsc_async_services* services;
    uint64_t op_id;
    Phase phase;
};

}  // namespace

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_get_descriptor(const yaddnsc_driver_descriptor** out) {
    if (out == nullptr) {
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    *out = &DESCRIPTOR;
    return YADDNSC_STATUS_OK;
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_async_create(
    const yaddnsc_async_services* services, yaddnsc_async_driver** out_driver, yaddnsc_error* out_error) {
    if (out_driver == nullptr) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, BAD_ARGUMENT);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    *out_driver = nullptr;
    if (!valid_services(services)) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, BAD_ARGUMENT);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    auto* state = new (std::nothrow) AsyncState{services, 0, Phase::Initial};
    if (state == nullptr) {
        write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, BAD_ARGUMENT);
        return YADDNSC_STATUS_INTERNAL_ERROR;
    }
    *out_driver = reinterpret_cast<yaddnsc_async_driver*>(state);  // NOLINT
    return YADDNSC_STATUS_OK;
}

extern "C" FIXTURE_EXPORT void yaddnsc_driver_async_destroy(yaddnsc_async_driver* driver) {
    delete reinterpret_cast<AsyncState*>(driver);  // NOLINT — nullptr is a no-op
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_update_async_begin(
    yaddnsc_async_driver* driver, const yaddnsc_update_request* request, yaddnsc_error* out_error) {
    auto* state = reinterpret_cast<AsyncState*>(driver);  // NOLINT
    if (state == nullptr || state->phase != Phase::Initial) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, BAD_ARGUMENT);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    if (!valid_request(request)) {
        state->phase = Phase::UpdateTerminal;
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, BAD_ARGUMENT);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    if (state->services->is_cancelled(state->services->context) != 0) {
        state->phase = Phase::UpdateTerminal;
        write_error(out_error, YADDNSC_STATUS_CANCELLED, CANCELLED_MESSAGE);
        return YADDNSC_STATUS_CANCELLED;
    }

    // The test passes the gateway server URL as driver_param_json; the host
    // copies it during http_start.
    const std::string_view configured =
        request->driver_param_json.data != nullptr && request->driver_param_json.size != 0
            ? std::string_view(reinterpret_cast<const char*>(request->driver_param_json.data),
                               request->driver_param_json.size)
            : std::string_view{};
    std::string_view url = configured;
    bool cancel_same_step = false;
    if (const auto separator = configured.find('|'); separator != std::string_view::npos) {
        url = configured.substr(0, separator);
        cancel_same_step = configured.substr(separator + 1) == "cancel";
    }
    if (!url.starts_with("http://") && !url.starts_with("https://")) {
        url = "http://127.0.0.1/async";
    }
    const yaddnsc_http_request http_request{
        .struct_size = static_cast<uint32_t>(sizeof(yaddnsc_http_request)),
        .url = {url.data(), url.size()},
        .method = YADDNSC_HTTP_GET,
        .headers = nullptr,
        .header_count = 0,
        .content_type = {},
        .body = {},
    };
    uint64_t op_id = 0;
    const yaddnsc_status start =
        state->services->http_start(state->services->context, &http_request, &op_id, out_error);
    if (start != YADDNSC_STATUS_OK) {
        state->phase = Phase::UpdateTerminal;
        return start;
    }
    if (op_id == 0) {
        state->phase = Phase::UpdateTerminal;
        write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, ZERO_OP_MESSAGE);
        return YADDNSC_STATUS_INTERNAL_ERROR;
    }
    if (cancel_same_step) {
        yaddnsc_error cancel_error{};
        cancel_error.struct_size = static_cast<uint32_t>(sizeof(cancel_error));
        const yaddnsc_status cancel_status =
            state->services->http_cancel(state->services->context, op_id, &cancel_error);
        if (cancel_status != YADDNSC_STATUS_OK) {
            state->phase = Phase::UpdateTerminal;
            return cancel_status;
        }
    }
    state->op_id = op_id;
    state->phase = Phase::UpdateWaiting;
    return YADDNSC_STATUS_PENDING;
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_update_async_resume(
    yaddnsc_async_driver* driver, const yaddnsc_event* event, yaddnsc_error* out_error) {
    auto* state = reinterpret_cast<AsyncState*>(driver);  // NOLINT
    if (state == nullptr || state->phase != Phase::UpdateWaiting || event == nullptr ||
        event->struct_size < YADDNSC_EVENT_MIN_SIZE || event->abi_major != YADDNSC_DRIVER_ABI_MAJOR ||
        event->abi_minor < 1) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, BAD_ARGUMENT);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }

    if (event->kind == YADDNSC_EVENT_UPDATE_CANCELLED) {
        if (event->op_id != 0 || event->status != YADDNSC_STATUS_CANCELLED ||
            !valid_error(event->error, YADDNSC_STATUS_CANCELLED) || event->response != nullptr) {
            write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, BAD_ARGUMENT);
            return YADDNSC_STATUS_INVALID_ARGUMENT;
        }
        state->phase = Phase::UpdateTerminal;
        state->op_id = 0;
        write_error(out_error, YADDNSC_STATUS_CANCELLED, CANCELLED_MESSAGE);
        return YADDNSC_STATUS_CANCELLED;
    }

    if (event->kind != YADDNSC_EVENT_HTTP_FINISHED || event->op_id != state->op_id) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, BAD_ARGUMENT);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }

    if (event->status == YADDNSC_STATUS_OK) {
        if (!valid_response(event->response) || event->error != nullptr) {
            write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, BAD_ARGUMENT);
            return YADDNSC_STATUS_INVALID_ARGUMENT;
        }
        state->phase = Phase::UpdateTerminal;
        state->op_id = 0;
        return YADDNSC_STATUS_OK;
    }

    if (!yaddnsc_status_is_valid(event->status) || event->status == YADDNSC_STATUS_PENDING ||
        !valid_error(event->error, event->status) || event->response != nullptr) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, BAD_ARGUMENT);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    state->phase = Phase::UpdateTerminal;
    state->op_id = 0;
    write_error(out_error, event->status, EVENT_ERROR_MESSAGE);
    return event->status;
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_async_validate(
    yaddnsc_async_driver* driver, yaddnsc_string driver_param_json, yaddnsc_error* out_error) {
    if (driver == nullptr) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, BAD_ARGUMENT);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    auto* state = reinterpret_cast<AsyncState*>(driver);  // NOLINT
    if (state->phase != Phase::Initial) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, BAD_ARGUMENT);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    state->phase = Phase::ValidateTerminal;
    if (!yaddnsc_string_is_valid(driver_param_json)) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, BAD_ARGUMENT);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    if (driver_param_json.size == 0) {
        write_error(out_error, YADDNSC_STATUS_INVALID_CONFIG, EMPTY_CONFIG_MESSAGE);
        return YADDNSC_STATUS_INVALID_CONFIG;
    }
    return YADDNSC_STATUS_OK;
}
