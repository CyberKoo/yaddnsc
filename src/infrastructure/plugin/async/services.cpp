#include "services.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "application/ports/log.h"
#include "infrastructure/plugin/detail/abi_string.h"
#include "infrastructure/plugin/detail/cycle.h"
#include "yaddnsc/sdk/driver_abi.h"

namespace plugin {

namespace {

[[nodiscard]] app::LogLevel to_log_level(yaddnsc_log_level level) noexcept {
    switch (level) {
        case YADDNSC_LOG_TRACE:
            return app::LogLevel::TRACE;
        case YADDNSC_LOG_DEBUG:
            return app::LogLevel::DEBUG;
        case YADDNSC_LOG_INFO:
            return app::LogLevel::INFO;
        case YADDNSC_LOG_WARN:
            return app::LogLevel::WARN;
        case YADDNSC_LOG_ERROR:
            return app::LogLevel::ERROR;
        default:
            return app::LogLevel::INFO;
    }
}

[[nodiscard]] std::optional<http::Method> to_http_method(yaddnsc_http_method method) noexcept {
    switch (method) {
        case YADDNSC_HTTP_GET:
            return http::Method::GET;
        case YADDNSC_HTTP_POST:
            return http::Method::POST;
        case YADDNSC_HTTP_PUT:
            return http::Method::PUT;
        case YADDNSC_HTTP_DELETE:
            return http::Method::DELETE;
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

void write_error(yaddnsc_error* out_error, yaddnsc_status status, std::string_view message) noexcept {
    if (out_error == nullptr || out_error->struct_size < YADDNSC_ERROR_MIN_SIZE) {
        return;
    }
    out_error->status = status;
    out_error->retry_after_seconds = 0;
    out_error->message = {message.data(), message.size()};
    out_error->struct_size = out_error->struct_size < static_cast<uint32_t>(sizeof(yaddnsc_error))
                                 ? out_error->struct_size
                                 : static_cast<uint32_t>(sizeof(yaddnsc_error));
}

[[nodiscard]] bool valid_request(const yaddnsc_http_request& request) noexcept {
    // Never read a field outside the provider's declared prefix.
    if (request.struct_size < YADDNSC_HTTP_REQUEST_MIN_SIZE) {
        return false;
    }
    if (!yaddnsc_string_is_valid(request.url) || request.url.size == 0) {
        return false;
    }
    if (!to_http_method(request.method).has_value()) {
        return false;
    }
    if (!yaddnsc_http_header_array_is_valid(request.headers, request.header_count)) {
        return false;
    }
    if (!yaddnsc_string_is_valid(request.content_type) || !yaddnsc_bytes_is_valid(request.body)) {
        return false;
    }
    for (size_t i = 0; i < request.header_count; ++i) {
        if (!yaddnsc_http_header_is_valid(request.headers[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool copy_request(const yaddnsc_http_request& request, AsyncStartCommand& command) {
    const auto method = to_http_method(request.method);
    if (!method.has_value()) {
        return false;
    }
    command.url = std::string(detail::to_view(request.url));
    command.request.method = *method;
    for (size_t i = 0; i < request.header_count; ++i) {
        command.request.headers.emplace(std::string(detail::to_view(request.headers[i].name)),
                                        std::string(detail::to_view(request.headers[i].value)));
    }
    if (request.body.data != nullptr) {
        command.request.set_body(std::span<const std::uint8_t>(request.body.data, request.body.size));
    }
    command.request.content_type = std::string(detail::to_view(request.content_type));
    return true;
}

}  // namespace

HostAsyncServicesContext::HostAsyncServicesContext(const app::LoggerPort& logger, std::shared_ptr<CallState> state)
    : logger_(logger), state_(std::move(state)) {
    services_ = yaddnsc_async_services{
        .struct_size = static_cast<uint32_t>(sizeof(yaddnsc_async_services)),
        .abi_major = YADDNSC_DRIVER_ABI_MAJOR,
        .abi_minor = YADDNSC_DRIVER_ABI_MINOR,
        .context = this,
        .log = &log_entry,
        .is_cancelled = &is_cancelled_entry,
        .http_start = &http_start_entry,
        .http_cancel = &http_cancel_entry,
        .log_enabled = &log_enabled_entry,
    };
}

std::int32_t HostAsyncServicesContext::is_cancelled() const noexcept {
    return state_ != nullptr && state_->cancelled.load(std::memory_order_acquire) ? 1 : 0;
}

void HostAsyncServicesContext::log(yaddnsc_log_level level, yaddnsc_string message,
                                   const yaddnsc_source_location* location) {
    const app::LogLevel mapped = to_log_level(level);
    if (!logger_.is_enabled(mapped)) {
        return;
    }
    const std::string_view file =
        (location != nullptr && location->file.data != nullptr) ? detail::to_view(location->file) : std::string_view{};
    const int line = location != nullptr ? location->line : 0;
    const std::string_view function = (location != nullptr && location->function.data != nullptr)
                                          ? detail::to_view(location->function)
                                          : std::string_view{};
    logger_.log_explicit(mapped, detail::to_view(message), file, line, function);
}

std::int32_t HostAsyncServicesContext::log_enabled(yaddnsc_log_level level) const noexcept {
    try {
        return logger_.is_enabled(to_log_level(level)) ? 1 : 0;
    } catch (...) {
        return 1;
    }
}

yaddnsc_status HostAsyncServicesContext::http_start(const yaddnsc_http_request& request, uint64_t* out_op_id,
                                                    yaddnsc_error* out_error) {
    if (out_op_id == nullptr) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "out_op_id must not be null");
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    *out_op_id = 0;
    if (commands_ == nullptr) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "async HTTP command sink is not available");
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    if (phase_ != Phase::BEGIN && phase_ != Phase::RESUME) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT,
                    "http_start is only available during async begin/resume");
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    if (!valid_request(request)) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "invalid async HTTP request");
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    if (is_cancelled() != 0) {
        write_error(out_error, YADDNSC_STATUS_CANCELLED, "the request was cancelled");
        return YADDNSC_STATUS_CANCELLED;
    }

    AsyncStartCommand command;
    try {
        if (!copy_request(request, command)) {
            write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "invalid async HTTP request");
            return YADDNSC_STATUS_INVALID_ARGUMENT;
        }
    } catch (...) {
        write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, "async HTTP request copy failed");
        return YADDNSC_STATUS_INTERNAL_ERROR;
    }

    if (next_op_id_ == 0) {
        write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, "async op id space exhausted");
        return YADDNSC_STATUS_INTERNAL_ERROR;
    }
    command.op_id = next_op_id_++;
    active_ops_.insert(command.op_id);
    *out_op_id = command.op_id;
    try {
        commands_->starts.push_back(std::move(command));
    } catch (...) {
        active_ops_.erase(*out_op_id);
        *out_op_id = 0;
        write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, "async HTTP command copy failed");
        return YADDNSC_STATUS_INTERNAL_ERROR;
    }
    return YADDNSC_STATUS_OK;
}

yaddnsc_status HostAsyncServicesContext::http_cancel(uint64_t op_id, yaddnsc_error* out_error) {
    if (commands_ == nullptr) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "async HTTP command sink is not available");
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    if (phase_ != Phase::BEGIN && phase_ != Phase::RESUME) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT,
                    "http_cancel is only available during async begin/resume");
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    if (op_id == 0 || active_ops_.find(op_id) == active_ops_.end()) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "unknown or already-delivered async op");
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    if (std::find(commands_->cancels.begin(), commands_->cancels.end(), op_id) == commands_->cancels.end()) {
        try {
            commands_->cancels.push_back(op_id);
        } catch (...) {
            write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, "async HTTP cancel copy failed");
            return YADDNSC_STATUS_INTERNAL_ERROR;
        }
    }
    return YADDNSC_STATUS_OK;
}

void HostAsyncServicesContext::log_entry(void* context, yaddnsc_log_level level, yaddnsc_string message,
                                         const yaddnsc_source_location* location) noexcept {
    try {
        static_cast<HostAsyncServicesContext*>(context)->log(level, message, location);
    } catch (...) {
    }
}

std::int32_t HostAsyncServicesContext::log_enabled_entry(void* context, yaddnsc_log_level level) noexcept {
    if (context == nullptr) {
        return 0;
    }
    return static_cast<HostAsyncServicesContext*>(context)->log_enabled(level);
}

std::int32_t HostAsyncServicesContext::is_cancelled_entry(void* context) noexcept {
    return context == nullptr ? 0 : static_cast<HostAsyncServicesContext*>(context)->is_cancelled();
}

yaddnsc_status HostAsyncServicesContext::http_start_entry(void* context, const yaddnsc_http_request* request,
                                                          uint64_t* out_op_id, yaddnsc_error* out_error) noexcept {
    if (out_op_id != nullptr) {
        *out_op_id = 0;
    }
    if (context == nullptr || request == nullptr) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "context and request must not be null");
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    try {
        return static_cast<HostAsyncServicesContext*>(context)->http_start(*request, out_op_id, out_error);
    } catch (...) {
        write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, "unknown exception during async http_start");
        return YADDNSC_STATUS_INTERNAL_ERROR;
    }
}

yaddnsc_status HostAsyncServicesContext::http_cancel_entry(void* context, uint64_t op_id,
                                                           yaddnsc_error* out_error) noexcept {
    if (context == nullptr) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "context must not be null");
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    try {
        return static_cast<HostAsyncServicesContext*>(context)->http_cancel(op_id, out_error);
    } catch (...) {
        write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, "unknown exception during async http_cancel");
        return YADDNSC_STATUS_INTERNAL_ERROR;
    }
}

}  // namespace plugin
