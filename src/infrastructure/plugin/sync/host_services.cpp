//
// plugin — the per-call Host Services table over the coroutine bridge
// (implementation).
//

#include "host_services.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <expected>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <yaddnsc/sdk/driver_abi.h>

#include "application/ports/log.h"
#include "coro/cancelled.h"
#include "infrastructure/http/types.h"
#include "infrastructure/plugin/detail/abi_string.h"
#include "infrastructure/plugin/detail/host_services.h"
#include "infrastructure/plugin/sync/bridge.h"

namespace plugin {

namespace {

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

using detail::write_error;

}  // namespace

HostServicesContext::HostServicesContext(Bridge& bridge, const app::LoggerPort& logger,
                                         std::shared_ptr<CallState> state)
    : bridge_(bridge), logger_(logger), state_(std::move(state)) {}

std::string_view HostServicesContext::arena_copy(std::string_view value) {
    return string_arena_.emplace_back(value);
}

void HostServicesContext::log(yaddnsc_log_level level, yaddnsc_string message,
                              const yaddnsc_source_location* location) {
    detail::log_record(logger_, level, message, location);
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

std::int32_t HostServicesContext::log_enabled_entry(void* context, yaddnsc_log_level level) noexcept {
    if (context == nullptr) {
        return 0;
    }
    try {
        const auto* self = static_cast<HostServicesContext*>(context);
        return self->logger_.is_enabled(detail::to_log_level(level)) ? 1 : 0;
    } catch (...) {
        // Best effort: an unusable query must not drop a log record.
        return 1;
    }
}

yaddnsc_status HostServicesContext::http_exchange_entry(void* context, const yaddnsc_http_request* request,
                                                        yaddnsc_http_response* out_response, yaddnsc_error* out_error) {
    if (context == nullptr || request == nullptr || out_response == nullptr) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "context, request and out_response must not be null");
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
    if (static_cast<HostServicesContext*>(context)->phase_ != Phase::UPDATE) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT,
                    "http_exchange is only available during yaddnsc_driver_update");
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    // A host exception (including bad_alloc while copying into the arena)
    // must never escape into the plugin's C frame. Its reporting path uses a
    // fixed buffer, so handling allocation failure does not allocate again.
    try {
        return static_cast<HostServicesContext*>(context)->http_exchange(*request, out_response, out_error);
    } catch (const coro::Cancelled&) {
        write_error(out_error, YADDNSC_STATUS_CANCELLED, "the request was cancelled");
        return YADDNSC_STATUS_CANCELLED;
    } catch (const std::exception& e) {
        static_cast<HostServicesContext*>(context)->state_->record_worker_error(std::current_exception());
        write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, copy_callback_error(e.what()));
    } catch (...) {
        static_cast<HostServicesContext*>(context)->state_->record_worker_error(std::current_exception());
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
    const auto method = detail::to_http_method(request.method);
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
        write_error(out_error, YADDNSC_STATUS_CANCELLED, "the request was cancelled");
        return YADDNSC_STATUS_CANCELLED;
    }

    // Build the request the bridge will carry to the loop, then block the
    // worker on the loop-side answer.
    auto call = std::make_shared<BridgeCall>();
    call->url = std::string(detail::to_view(request.url));
    call->request.method = *method;
    for (size_t i = 0; i < request.header_count; ++i) {
        call->request.headers.emplace(std::string(detail::to_view(request.headers[i].name)),
                                      std::string(detail::to_view(request.headers[i].value)));
    }
    if (request.body.data != nullptr) {
        call->request.set_body(std::span<const std::uint8_t>(request.body.data, request.body.size));
    }
    call->request.content_type = std::string(detail::to_view(request.content_type));

    call->cycle = state_;
    auto response = bridge_.exchange(call);

    if (!response) {
        const auto& error = response.error();
        // The bridge translates cancellation, transport failures and defects
        // into the existing ABI status vocabulary before fulfilling the promise.
        const yaddnsc_status status = error.code;
        if (out_error != nullptr && out_error->struct_size >= YADDNSC_ERROR_MIN_SIZE) {
            write_error(out_error, status, arena_copy(error.message), error.retry_after_seconds);
        }
        return status;
    }

    // Fill the caller-provided response struct; every view points into this
    // context's arena and stays valid until the cycle returns.
    out_response->status_code = static_cast<uint32_t>(response->status_);
    const std::string_view body = arena_copy(response->text());
    out_response->body = yaddnsc_bytes{reinterpret_cast<const uint8_t*>(body.data()), body.size()};

    auto& header_array = header_array_arena_.emplace_back();
    header_array.reserve(response->headers_.size());
    for (const auto& [name, value] : response->headers_) {
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
