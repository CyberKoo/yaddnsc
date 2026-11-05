#include "host_services.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>

#include <yaddnsc/sdk/driver_abi.h>

#include "application/ports/log.h"
#include "infrastructure/http/types.h"
#include "infrastructure/plugin/detail/abi_string.h"

namespace plugin::detail {

app::LogLevel to_log_level(yaddnsc_log_level level) noexcept {
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

std::optional<http::Method> to_http_method(yaddnsc_http_method method) noexcept {
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

void write_error(yaddnsc_error* out_error, yaddnsc_status status, std::string_view message,
                 uint32_t retry_after_seconds) noexcept {
    if (out_error == nullptr || out_error->struct_size < YADDNSC_ERROR_MIN_SIZE) {
        return;
    }
    out_error->status = status;
    out_error->retry_after_seconds = retry_after_seconds;
    out_error->message = {message.data(), message.size()};
    out_error->struct_size = std::min(out_error->struct_size, static_cast<uint32_t>(sizeof(yaddnsc_error)));
}

void log_record(const app::LoggerPort& logger, yaddnsc_log_level level, yaddnsc_string message,
                const yaddnsc_source_location* location) {
    const app::LogLevel mapped = to_log_level(level);
    if (!logger.is_enabled(mapped)) {
        return;
    }
    const std::string_view file =
        (location != nullptr && location->file.data != nullptr) ? to_view(location->file) : std::string_view{};
    const int line = location != nullptr ? location->line : 0;
    const std::string_view function =
        (location != nullptr && location->function.data != nullptr) ? to_view(location->function) : std::string_view{};
    logger.log_explicit(mapped, to_view(message), file, line, function);
}

}  // namespace plugin::detail
