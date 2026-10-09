//
// plugin — the per-call Host Services table over the coroutine bridge
// (implementation).
//

#include "host_services.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <expected>
#include <stddef.h>
#include <yaddnsc/sdk/driver_abi.h>

#include "application/ports/log.h"
#include "infrastructure/net/http/error.h"
#include "infrastructure/net/http/types.h"

namespace plugin {

namespace {

[[nodiscard]] std::string_view to_view(yaddnsc_string value) noexcept {
    return value.data == nullptr ? std::string_view{} : std::string_view{value.data, value.size};
}

/// The C callback firewall must not allocate while reporting an exception:
/// allocation failure is precisely one of the cases it is handling.
[[nodiscard]] std::string_view copy_callback_error(std::string_view message) noexcept {
    constexpr std::string_view fallback = "host exception during HTTP exchange";
    constexpr std::size_t capacity = 512;
    thread_local std::array<char, capacity> storage{};
    const std::string_view source = message.data() == nullptr ? fallback : message;
    const std::size_t size = std::min(source.size(), storage.size() - 1);
    if (size != 0) {
        std::memcpy(storage.data(), source.data(), size);
    }
    storage[size] = '\0';
    return {storage.data(), size};
}

/// Write an error report honouring the caller-supplied capacity (same
/// rules as the SDK side): fields are only written when fully covered,
/// struct_size is written back as min(capacity, sizeof), and an unknown
/// tail is never zeroed.
void write_error(yaddnsc_error* out_error, yaddnsc_status status, std::string_view message,
                 uint32_t retry_after_seconds = 0) noexcept {
    if (out_error == nullptr || out_error->struct_size < YADDNSC_ERROR_MIN_SIZE) {
        return;
    }
    out_error->status = status;
    out_error->retry_after_seconds = retry_after_seconds;
    out_error->message = yaddnsc_string{message.data(), message.size()};
    out_error->struct_size = std::min(out_error->struct_size, static_cast<uint32_t>(sizeof(yaddnsc_error)));
}

[[nodiscard]] LogLevel to_log_level(yaddnsc_log_level level) noexcept {
    switch (level) {
        case YADDNSC_LOG_TRACE:
            return LogLevel::TRACE;
        case YADDNSC_LOG_DEBUG:
            return LogLevel::DEBUG;
        case YADDNSC_LOG_INFO:
            return LogLevel::INFO;
        case YADDNSC_LOG_WARN:
            return LogLevel::WARN;
        case YADDNSC_LOG_ERROR:
            return LogLevel::ERROR;
        default:
            return LogLevel::INFO;
    }
}

/// Map an ABI method constant onto the http verb; nullopt when the constant
/// is not one of the eight defined methods.
[[nodiscard]] std::optional<http::Method> to_http_method(yaddnsc_http_method method) noexcept {
    switch (method) {
        case YADDNSC_HTTP_GET:
            return http::Method::GET;
        case YADDNSC_HTTP_POST:
            return http::Method::POST;
        case YADDNSC_HTTP_PUT:
            return http::Method::PUT;
        case YADDNSC_HTTP_DELETE:
            return http::Method::DEL;
        case YADDNSC_HTTP_PATCH:
            return http::Method::PATCH;
        case YADDNSC_HTTP_HEAD:
            return http::Method::HEAD;
        case YADDNSC_HTTP_OPTIONS:
            return http::Method::OPTIONS;
        default:
            return std::nullopt;
    }
}

}  // namespace

HostServicesContext::HostServicesContext(Bridge& bridge, const Logger& logger, std::shared_ptr<CallState> state)
    : bridge_(bridge), logger_(logger), state_(std::move(state)) {}

std::string_view HostServicesContext::arena_copy(std::string_view value) {
    return string_arena_.emplace_back(value);
}

void HostServicesContext::log(yaddnsc_log_level level, yaddnsc_string message,
                              const yaddnsc_source_location* location) {
    // Contract: location must be non-null with non-empty file/function views
    // and line > 0. Tolerate violations defensively — logging must never
    // fail an update.
    const std::string_view file =
        (location != nullptr && location->file.data != nullptr) ? to_view(location->file) : std::string_view{};
    const int line = location != nullptr ? location->line : 0;
    const std::string_view function =
        (location != nullptr && location->function.data != nullptr) ? to_view(location->function) : std::string_view{};
    logger_.log_explicit(to_log_level(level), to_view(message), file, line, function);
}

void HostServicesContext::log_entry(void* context, yaddnsc_log_level level, yaddnsc_string message,
                                    const yaddnsc_source_location* location) noexcept {
    // Contract: logging failure must never fail an update — and a host
    // exception (e.g. bad_alloc) must never escape into the plugin's
    // C frame.
    try {
        static_cast<HostServicesContext*>(context)->log(level, message, location);
    } catch (...) {
    }
}

yaddnsc_status HostServicesContext::http_exchange_unavailable_entry(void*, const yaddnsc_http_request*,
                                                                     yaddnsc_http_response*, yaddnsc_error* out_error) {
    write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT,
                "http_exchange is only available during yaddnsc_driver_update");
    return YADDNSC_STATUS_INVALID_ARGUMENT;
}

yaddnsc_status HostServicesContext::http_exchange_entry(void* context, const yaddnsc_http_request* request,
                                                        yaddnsc_http_response* out_response, yaddnsc_error* out_error) {
    if (context == nullptr || request == nullptr || out_response == nullptr || out_error == nullptr) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT,
                    "context, request, out_response and out_error must not be null");
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    if (request->struct_size < YADDNSC_HTTP_REQUEST_MIN_SIZE) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "http request struct_size too small");
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    if (out_response->struct_size < YADDNSC_HTTP_RESPONSE_MIN_SIZE) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "http response struct_size too small");
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    if (out_error->struct_size < YADDNSC_ERROR_MIN_SIZE) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "error struct_size too small");
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    // A host exception (including bad_alloc while copying into the arena)
    // must never escape into the plugin's C frame. Its reporting path uses a
    // fixed buffer, so handling allocation failure does not allocate again.
    try {
        return static_cast<HostServicesContext*>(context)->http_exchange(*request, out_response, out_error);
    } catch (const std::exception& e) {
        write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, copy_callback_error(e.what()));
    } catch (...) {
        write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, "unknown host exception during http exchange");
    }
    return YADDNSC_STATUS_INTERNAL_ERROR;
}

yaddnsc_status HostServicesContext::http_exchange(const yaddnsc_http_request& request,
                                                  yaddnsc_http_response* out_response, yaddnsc_error* out_error) {
    if (!yaddnsc_string_is_valid(request.url) || request.url.size == 0) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "request url must not be empty");
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    const auto method = to_http_method(request.method);
    if (!method.has_value()) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "unknown http method");
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    if (!yaddnsc_http_header_array_is_valid(request.headers, request.header_count)) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "header_count > 0 with null headers");
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    if (!yaddnsc_string_is_valid(request.content_type)) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "invalid content_type view");
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    if (!yaddnsc_bytes_is_valid(request.body)) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "body size > 0 with null body data");
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < request.header_count; ++i) {
        if (!yaddnsc_http_header_is_valid(request.headers[i])) {
            write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "invalid HTTP header view");
            return YADDNSC_STATUS_INVALID_ARGUMENT;
        }
    }
    if (is_cancelled() != 0) {
        write_error(out_error, YADDNSC_STATUS_CANCELLED, "HTTP exchange cancelled");
        return YADDNSC_STATUS_CANCELLED;
    }

    // Build the request the bridge will carry to the loop, then block the
    // worker on the loop-side answer.
    auto call = std::make_shared<BridgeCall>();
    call->url = std::string(to_view(request.url));
    call->request.method = *method;
    for (size_t i = 0; i < request.header_count; ++i) {
        call->request.headers.emplace(std::string(to_view(request.headers[i].name)),
                                      std::string(to_view(request.headers[i].value)));
    }
    if (request.body.data != nullptr) {
        call->request.set_body(std::span<const std::uint8_t>(request.body.data, request.body.size));
    }
    call->request.content_type = std::string(to_view(request.content_type));

    state_->in_flight.store(call, std::memory_order_release);
    auto response = bridge_.exchange(call);
    state_->in_flight.store(nullptr, std::memory_order_release);

    if (!response) {
        const auto& error = response.error();
        // The ABI surface is deliberately coarser than http's error model:
        // only cancellation is distinguishable, and every other failure (TLS,
        // connect, parse, size limits) becomes NETWORK_ERROR. The diagnosis
        // still reaches the plugin in the message text — which the driver
        // gateway forwards into DriverError, so the user sees it. Widening the
        // status set means new yaddnsc_status values, i.e. an ABI change, not a
        // local refactor: do not expand this ternary without bumping
        // YADDNSC_DRIVER_ABI_MAJOR.
        const yaddnsc_status status =
            error.code == http::ErrorCode::CANCELLED ? YADDNSC_STATUS_CANCELLED : YADDNSC_STATUS_NETWORK_ERROR;
        write_error(out_error, status, arena_copy(error.message), error.retry_after_seconds);
        return status;
    }

    // Fill the caller-provided response struct; every view points into this
    // context's arena and stays valid until the cycle returns.
    out_response->status_code = static_cast<uint32_t>(response->status);
    const std::string_view body = arena_copy(response->text());
    out_response->body = yaddnsc_bytes{reinterpret_cast<const uint8_t*>(body.data()), body.size()};

    auto& header_array = header_array_arena_.emplace_back();
    header_array.reserve(response->headers.size());
    for (const auto& [name, value] : response->headers) {
        header_array.push_back({yaddnsc_string{arena_copy(name).data(), name.size()},
                                yaddnsc_string{arena_copy(value).data(), value.size()}});
    }
    out_response->headers = header_array.data();
    out_response->header_count = header_array.size();
    out_response->struct_size =
        std::min(out_response->struct_size, static_cast<uint32_t>(sizeof(yaddnsc_http_response)));
    return YADDNSC_STATUS_OK;
}

}  // namespace plugin
