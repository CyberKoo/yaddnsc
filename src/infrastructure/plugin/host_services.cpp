//
// Created by Kotarou on 2026/9/17.
//

#include "host_services.h"

#include <algorithm>
#include <expected>

#include "interface/http_client.h"

namespace {
    [[nodiscard]] std::string_view to_view(yaddnsc_string value) noexcept {
        return {value.data, value.size};
    }

    /// Write an error report honouring the caller-supplied capacity (same
    /// rules as the SDK side): fields are only written when fully covered,
    /// struct_size is written back as min(capacity, sizeof), and an unknown
    /// tail is never zeroed.
    void write_error(yaddnsc_error *out_error, yaddnsc_status status, std::string_view message) noexcept {
        if (out_error == nullptr || out_error->struct_size < YADDNSC_ERROR_MIN_SIZE) {
            return;
        }
        out_error->status = status;
        out_error->retry_after_seconds = 0;
        out_error->message = yaddnsc_string{message.data(), message.size()};
        out_error->struct_size = std::min(out_error->struct_size, static_cast<uint32_t>(sizeof(yaddnsc_error)));
    }

    [[nodiscard]] LogLevel to_log_level(yaddnsc_log_level level) noexcept {
        switch (level) {
            case YADDNSC_LOG_TRACE:
                return LogLevel::trace;
            case YADDNSC_LOG_DEBUG:
                return LogLevel::debug;
            case YADDNSC_LOG_INFO:
                return LogLevel::info;
            case YADDNSC_LOG_WARN:
                return LogLevel::warn;
            case YADDNSC_LOG_ERROR:
                return LogLevel::error;
            default:
                return LogLevel::info;
        }
    }

    /// Map an ABI method constant onto the net::http verb; nullopt when the
    /// constant is not one of the eight defined methods.
    [[nodiscard]] std::optional<net::http::Method> to_http_method(yaddnsc_http_method method) noexcept {
        switch (method) {
            case YADDNSC_HTTP_GET:
                return net::http::Method::GET;
            case YADDNSC_HTTP_POST:
                return net::http::Method::POST;
            case YADDNSC_HTTP_PUT:
                return net::http::Method::PUT;
            case YADDNSC_HTTP_DELETE:
                return net::http::Method::DEL;
            case YADDNSC_HTTP_PATCH:
                return net::http::Method::PATCH;
            case YADDNSC_HTTP_HEAD:
                return net::http::Method::HEAD;
            case YADDNSC_HTTP_OPTIONS:
                return net::http::Method::OPTIONS;
            default:
                return std::nullopt;
        }
    }
} // anonymous namespace

HostServicesContext::HostServicesContext(HttpClient &http_client, const Logger &logger,
                                         Utils::CancellationToken cancel_token)
    : http_client_(http_client), logger_(logger), cancel_token_(std::move(cancel_token)) {
}

std::string_view HostServicesContext::arena_copy(std::string_view value) {
    return string_arena_.emplace_back(value);
}

void HostServicesContext::log(yaddnsc_log_level level, yaddnsc_string message,
                              const yaddnsc_source_location *location) {
    // Contract: location must be non-null with non-empty file/function views
    // and line > 0. Tolerate violations defensively — logging must never
    // fail an update.
    const std::string_view file =
            (location != nullptr && location->file.data != nullptr) ? to_view(location->file) : std::string_view{};
    const int line = location != nullptr ? location->line : 0;
    const std::string_view function =
            (location != nullptr && location->function.data != nullptr) ? to_view(location->function)
                                                                        : std::string_view{};
    logger_.log_explicit(to_log_level(level), to_view(message), file, line, function);
}

yaddnsc_status HostServicesContext::http_exchange_entry(void *context, const yaddnsc_http_request *request,
                                                        yaddnsc_http_response *out_response,
                                                        yaddnsc_error *out_error) {
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
    return static_cast<HostServicesContext *>(context)->http_exchange(*request, out_response, out_error);
}

yaddnsc_status HostServicesContext::http_exchange(const yaddnsc_http_request &request,
                                                  yaddnsc_http_response *out_response, yaddnsc_error *out_error) {
    if (request.url.data == nullptr || request.url.size == 0) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "request url must not be empty");
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    const auto method = to_http_method(request.method);
    if (!method.has_value()) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "unknown http method");
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    if (request.header_count > 0 && request.headers == nullptr) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "header_count > 0 with null headers");
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    if (request.body.size > 0 && request.body.data == nullptr) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "body size > 0 with null body data");
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }

    net::http::Request http_request;
    http_request.method = *method;
    for (size_t i = 0; i < request.header_count; ++i) {
        http_request.headers.emplace(std::string(to_view(request.headers[i].name)),
                                     std::string(to_view(request.headers[i].value)));
    }
    if (request.body.data != nullptr) {
        http_request.set_body(std::span<const std::uint8_t>(request.body.data, request.body.size));
    }
    http_request.content_type = std::string(to_view(request.content_type));

    auto response = http_client_.exchange(to_view(request.url), http_request);
    if (!response) {
        const auto &error = response.error();
        const yaddnsc_status status = error.code == net::http::ErrorCode::CANCELLED ? YADDNSC_STATUS_CANCELLED
                                                                                    : YADDNSC_STATUS_NETWORK_ERROR;
        write_error(out_error, status, arena_copy(error.message));
        return status;
    }

    // Fill the caller-provided response struct; every view points into this
    // context's arena and stays valid until the update call returns.
    out_response->status_code = static_cast<uint32_t>(response->status);
    const std::string_view body = arena_copy(response->text());
    out_response->body = yaddnsc_bytes{reinterpret_cast<const uint8_t *>(body.data()), body.size()};

    auto &header_array = header_array_arena_.emplace_back();
    header_array.reserve(response->headers.size());
    for (const auto &[name, value]: response->headers) {
        header_array.push_back(
                {yaddnsc_string{arena_copy(name).data(), name.size()},
                 yaddnsc_string{arena_copy(value).data(), value.size()}});
    }
    out_response->headers = header_array.data();
    out_response->header_count = header_array.size();
    out_response->struct_size =
            std::min(out_response->struct_size, static_cast<uint32_t>(sizeof(yaddnsc_http_response)));
    return YADDNSC_STATUS_OK;
}
