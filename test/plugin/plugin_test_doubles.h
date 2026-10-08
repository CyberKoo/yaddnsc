//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_TEST_PLUGIN_PLUGIN_TEST_DOUBLES_H
#define YADDNSC_TEST_PLUGIN_PLUGIN_TEST_DOUBLES_H

/// Shared test doubles for the v1 alpha plugin contract tests.
///
/// A scripted host-services table (log / http_exchange / is_cancelled) that
/// mirrors the ABI's memory and struct_size rules, a recording logger, and
/// run_module_cycle(), which drives one create → update → destroy cycle
/// through a dlopen'ed PluginModule.
///
/// The table is test-only scaffolding: the production host-services table is
/// the coroutine one (src/infrastructure/plugin/coro/host_services.h).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <stdint.h>
#include <yaddnsc/sdk/driver_abi.h>

#include "application/ports/log.h"
#include "infrastructure/plugin/plugin_loader.h"

/// HTTP verbs as the test can observe them (the ABI constants are mapped by
/// the host-services table before a request reaches the scripted transport).
enum class TestHttpMethod { GET, POST, PUT, DEL, PATCH, HEAD, OPTIONS };

/// One queued answer of the scripted transport.
struct ScriptedExchange {
    enum class Kind { RESPONSE, FAILURE };
    Kind kind = Kind::RESPONSE;
    int status = 0;
    std::string body;
    std::multimap<std::string, std::string> headers;
    bool cancelled = false;  ///< FAILURE variant: CANCELLED vs NETWORK_ERROR
    std::string message;
    std::uint32_t retry_after = 0;
};

/// Scripted transport: replays queued responses/errors and keeps an owned copy
/// of every captured request.
class ScriptedHttpTransport {
public:
    struct CapturedRequest {
        std::string url;
        TestHttpMethod method = TestHttpMethod::GET;
        std::multimap<std::string, std::string> headers;
        std::optional<std::string> body;  ///< nullopt = no body at all
        std::string content_type;
    };

    void queue_response(int status_code, std::string body, std::multimap<std::string, std::string> headers = {}) {
        queue_.push_back(ScriptedExchange{.kind = ScriptedExchange::Kind::RESPONSE,
                                          .status = status_code,
                                          .body = std::move(body),
                                          .headers = std::move(headers),
                                          .cancelled = false,
                                          .message = {},
                                          .retry_after = 0});
    }

    void queue_error(bool cancelled, std::string message, std::uint32_t retry_after_seconds = 0) {
        queue_.push_back(ScriptedExchange{.kind = ScriptedExchange::Kind::FAILURE,
                                          .status = 0,
                                          .body = {},
                                          .headers = {},
                                          .cancelled = cancelled,
                                          .message = std::move(message),
                                          .retry_after = retry_after_seconds});
    }

    /// Record the request and return the next queued answer. With an empty
    /// queue it behaves like a transport that could not connect.
    [[nodiscard]] ScriptedExchange take(CapturedRequest request) {
        requests_.push_back(std::move(request));
        if (queue_.empty()) {
            return ScriptedExchange{.kind = ScriptedExchange::Kind::FAILURE,
                                    .status = 0,
                                    .body = {},
                                    .headers = {},
                                    .cancelled = false,
                                    .message = "scripted transport: no queued exchange",
                                    .retry_after = 0};
        }
        auto front = std::move(queue_.front());
        queue_.pop_front();
        return front;
    }

    [[nodiscard]] std::vector<CapturedRequest> requests() const { return requests_; }

    [[nodiscard]] std::size_t request_count() const { return requests_.size(); }

    [[nodiscard]] std::size_t remaining() const { return queue_.size(); }

private:
    std::deque<ScriptedExchange> queue_;
    std::vector<CapturedRequest> requests_;
};

/// Logger double: records every record with its (explicit) source location.
/// Every level is enabled.
class RecordingLogger final : public Logger {
public:
    struct Record {
        LogLevel level;
        std::string message;
        std::string file;
        int line;
        std::string function;
    };

    [[nodiscard]] bool is_enabled(LogLevel) const override { return true; }

    void log(LogLevel level, std::string_view message, const std::source_location& loc) const override {
        records_.push_back(
            Record{level, std::string(message), loc.file_name(), static_cast<int>(loc.line()), loc.function_name()});
    }

    void log_explicit(LogLevel level, std::string_view message, std::string_view file, int line,
                      std::string_view function) const override {
        records_.push_back(Record{level, std::string(message), std::string(file), line, std::string(function)});
    }

    [[nodiscard]] std::vector<Record> records() const { return records_; }

private:
    mutable std::vector<Record> records_;
};

/// The host-services table used by the ABI contract tests. It mirrors the ABI
/// memory rules: every view a plugin reads points into the arena and stays
/// valid until this object dies.
class TestHostServices {
public:
    TestHostServices(RecordingLogger& logger, ScriptedHttpTransport& transport) : logger_(logger), transport_(transport) {}

    /// @param http_enabled  When false, http_exchange refuses (config-test path).
    [[nodiscard]] yaddnsc_host_services make_services(bool http_enabled = true) noexcept {
        return yaddnsc_host_services{
            .struct_size = static_cast<std::uint32_t>(sizeof(yaddnsc_host_services)),
            .abi_major = YADDNSC_DRIVER_ABI_MAJOR,
            .abi_minor = YADDNSC_DRIVER_ABI_MINOR,
            .context = this,
            .log = &log_entry,
            .http_exchange = http_enabled ? &http_exchange_entry : &http_exchange_unavailable_entry,
            .is_cancelled = &is_cancelled_entry,
        };
    }

    /// When set, is_cancelled() reports 1 and http_exchange refuses.
    bool cancelled = false;

private:
    [[nodiscard]] static std::string_view to_view(yaddnsc_string value) noexcept {
        return value.data == nullptr ? std::string_view{} : std::string_view{value.data, value.size};
    }

    [[nodiscard]] static std::optional<TestHttpMethod> to_method(yaddnsc_http_method method) noexcept {
        switch (method) {
            case YADDNSC_HTTP_GET:
                return TestHttpMethod::GET;
            case YADDNSC_HTTP_POST:
                return TestHttpMethod::POST;
            case YADDNSC_HTTP_PUT:
                return TestHttpMethod::PUT;
            case YADDNSC_HTTP_DELETE:
                return TestHttpMethod::DEL;
            case YADDNSC_HTTP_PATCH:
                return TestHttpMethod::PATCH;
            case YADDNSC_HTTP_HEAD:
                return TestHttpMethod::HEAD;
            case YADDNSC_HTTP_OPTIONS:
                return TestHttpMethod::OPTIONS;
            default:
                return std::nullopt;
        }
    }

    static void write_error(yaddnsc_error* out_error, yaddnsc_status status, std::string_view message,
                            std::uint32_t retry_after_seconds = 0) noexcept {
        if (out_error == nullptr || out_error->struct_size < YADDNSC_ERROR_MIN_SIZE) {
            return;
        }
        out_error->status = status;
        out_error->retry_after_seconds = retry_after_seconds;
        out_error->message = yaddnsc_string{message.data(), message.size()};
        out_error->struct_size = std::min(out_error->struct_size, static_cast<std::uint32_t>(sizeof(yaddnsc_error)));
    }

    [[nodiscard]] std::string_view arena_copy(std::string_view value) { return string_arena_.emplace_back(value); }

    static void log_entry(void* context, yaddnsc_log_level level, yaddnsc_string message,
                          const yaddnsc_source_location* location) noexcept {
        auto* self = static_cast<TestHostServices*>(context);
        if (self == nullptr) {
            return;
        }
        const std::string_view file =
            (location != nullptr && location->file.data != nullptr) ? to_view(location->file) : std::string_view{};
        const int line = location != nullptr ? location->line : 0;
        const std::string_view function = (location != nullptr && location->function.data != nullptr)
                                              ? to_view(location->function)
                                              : std::string_view{};
        const LogLevel mapped = level == YADDNSC_LOG_TRACE   ? LogLevel::TRACE
                                : level == YADDNSC_LOG_DEBUG ? LogLevel::DEBUG
                                : level == YADDNSC_LOG_WARN  ? LogLevel::WARN
                                : level == YADDNSC_LOG_ERROR ? LogLevel::ERROR
                                                             : LogLevel::INFO;
        try {
            self->logger_.log_explicit(mapped, to_view(message), file, line, function);
        } catch (...) {
        }
    }

    static std::int32_t is_cancelled_entry(void* context) noexcept {
        const auto* self = static_cast<TestHostServices*>(context);
        return self != nullptr && self->cancelled ? 1 : 0;
    }

    static yaddnsc_status http_exchange_unavailable_entry(void*, const yaddnsc_http_request*, yaddnsc_http_response*,
                                                          yaddnsc_error* out_error) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT,
                    "http_exchange is only available during yaddnsc_driver_update");
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }

    static yaddnsc_status http_exchange_entry(void* context, const yaddnsc_http_request* request,
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
        return static_cast<TestHostServices*>(context)->http_exchange(*request, out_response, out_error);
    }

    yaddnsc_status http_exchange(const yaddnsc_http_request& request, yaddnsc_http_response* out_response,
                                 yaddnsc_error* out_error) {
        if (!yaddnsc_string_is_valid(request.url) || request.url.size == 0) {
            write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "request url must not be empty");
            return YADDNSC_STATUS_INVALID_ARGUMENT;
        }
        const auto method = to_method(request.method);
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
        for (std::size_t i = 0; i < request.header_count; ++i) {
            if (!yaddnsc_http_header_is_valid(request.headers[i])) {
                write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "invalid HTTP header view");
                return YADDNSC_STATUS_INVALID_ARGUMENT;
            }
        }
        if (cancelled) {
            write_error(out_error, YADDNSC_STATUS_CANCELLED, "HTTP exchange cancelled");
            return YADDNSC_STATUS_CANCELLED;
        }

        ScriptedHttpTransport::CapturedRequest captured;
        captured.url = std::string(to_view(request.url));
        captured.method = *method;
        for (std::size_t i = 0; i < request.header_count; ++i) {
            captured.headers.emplace(std::string(to_view(request.headers[i].name)),
                                     std::string(to_view(request.headers[i].value)));
        }
        if (request.body.data != nullptr) {
            captured.body = std::string(reinterpret_cast<const char*>(request.body.data), request.body.size);
        }
        captured.content_type = std::string(to_view(request.content_type));

        ScriptedExchange answer = transport_.take(std::move(captured));
        if (answer.kind == ScriptedExchange::Kind::FAILURE) {
            const yaddnsc_status status = answer.cancelled ? YADDNSC_STATUS_CANCELLED : YADDNSC_STATUS_NETWORK_ERROR;
            write_error(out_error, status, arena_copy(answer.message), answer.retry_after);
            return status;
        }

        out_response->status_code = static_cast<std::uint32_t>(answer.status);
        const std::string_view body = arena_copy(answer.body);
        out_response->body = yaddnsc_bytes{reinterpret_cast<const std::uint8_t*>(body.data()), body.size()};

        auto& header_array = header_array_arena_.emplace_back();
        header_array.reserve(answer.headers.size());
        for (const auto& [name, value] : answer.headers) {
            header_array.push_back({yaddnsc_string{arena_copy(name).data(), name.size()},
                                    yaddnsc_string{arena_copy(value).data(), value.size()}});
        }
        out_response->headers = header_array.data();
        out_response->header_count = header_array.size();
        out_response->struct_size =
            std::min(out_response->struct_size, static_cast<std::uint32_t>(sizeof(yaddnsc_http_response)));
        return YADDNSC_STATUS_OK;
    }

    RecordingLogger& logger_;
    ScriptedHttpTransport& transport_;
    std::deque<std::string> string_arena_;
    std::deque<std::vector<yaddnsc_http_header>> header_array_arena_;
};

/// One per-update host-services context plus its scripted transport.
struct HostUpdateContext {
    RecordingLogger logger;
    ScriptedHttpTransport client;
    TestHostServices context;

    HostUpdateContext() : context(logger, client) {}

    [[nodiscard]] yaddnsc_host_services services() noexcept { return context.make_services(); }
};

/// Build a fully-populated update request (all views borrow the arguments).
[[nodiscard]] inline yaddnsc_update_request make_update_request(std::string_view ip_addr, std::string_view rd_type,
                                                                std::string_view domain, std::string_view subdomain,
                                                                std::string_view fqdn,
                                                                std::string_view driver_param_json) {
    return yaddnsc_update_request{
        .struct_size = static_cast<uint32_t>(sizeof(yaddnsc_update_request)),
        .ip_address = {ip_addr.data(), ip_addr.size()},
        .record_type = {rd_type.data(), rd_type.size()},
        .domain = {domain.data(), domain.size()},
        .subdomain = {subdomain.data(), subdomain.size()},
        .fqdn = {fqdn.data(), fqdn.size()},
        .driver_param_json = {reinterpret_cast<const uint8_t*>(driver_param_json.data()), driver_param_json.size()},
    };
}

/// The outcome of one create → update → destroy cycle through a PluginModule.
struct ModuleCycleResult {
    yaddnsc_status create_status = YADDNSC_STATUS_OK;
    yaddnsc_status update_status = YADDNSC_STATUS_OK;
    std::string error_message;
    uint32_t retry_after_seconds = 0;
};

/// Drive one full update cycle through a module's C entry points, copying
/// the error report out synchronously before destroy.
[[nodiscard]] inline ModuleCycleResult run_module_cycle(
    const PluginModule& module, const yaddnsc_host_services& services, std::string_view driver_param_json,
    std::string_view ip_addr = "192.0.2.1", std::string_view rd_type = "A", std::string_view domain = "example.com",
    std::string_view subdomain = "www", std::string_view fqdn = "www.example.com") {
    ModuleCycleResult result;
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));

    yaddnsc_driver* handle = nullptr;
    result.create_status = module.create(services, &handle, error);
    if (result.create_status != YADDNSC_STATUS_OK) {
        if (error.message.data != nullptr) {
            result.error_message = std::string(error.message.data, error.message.size);
        }
        return result;
    }

    const auto request = make_update_request(ip_addr, rd_type, domain, subdomain, fqdn, driver_param_json);
    result.update_status = module.update(handle, request, error);
    if (error.message.data != nullptr) {
        result.error_message = std::string(error.message.data, error.message.size);
    }
    result.retry_after_seconds = error.retry_after_seconds;

    module.destroy(handle);
    return result;
}

#endif  // YADDNSC_TEST_PLUGIN_PLUGIN_TEST_DOUBLES_H
