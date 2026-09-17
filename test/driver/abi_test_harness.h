//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_TEST_DRIVER_ABI_TEST_HARNESS_H
#define YADDNSC_TEST_DRIVER_ABI_TEST_HARNESS_H

/// Shared harness for driver tests on the v1 alpha ABI.
///
/// FakeHostServices implements the host services table in-process: it
/// captures every outgoing request (owned copies), replays queued
/// responses/transport errors, and records log records with source location.
/// run_abi_update() drives one full create → update → destroy cycle through
/// the driver's four C entry points, exactly as the production host does.

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <yaddnsc/sdk/driver_abi.h>

class FakeHostServices {
public:
    struct CapturedRequest {
        std::string url;
        yaddnsc_http_method method = 0;
        std::vector<std::pair<std::string, std::string>> headers;
        std::optional<std::string> body; ///< nullopt = no body at all
        std::string content_type;

        /// First value of the named header, or nullopt when absent.
        [[nodiscard]] std::optional<std::string> header(std::string_view name) const {
            for (const auto &[n, v]: headers) {
                if (n == name) {
                    return v;
                }
            }
            return std::nullopt;
        }
    };

    struct LogRecord {
        yaddnsc_log_level level;
        std::string message;
        std::string file;
        int32_t line;
        std::string function;
    };

    std::vector<CapturedRequest> requests;
    std::vector<LogRecord> logs;
    bool cancelled = false;

    /// Queue a successful exchange: the plugin receives OK + this response.
    void queue_response(uint32_t status_code, std::string body,
                        std::vector<std::pair<std::string, std::string>> headers = {}) {
        exchanges_.emplace_back(QueuedResponse{status_code, std::move(body), std::move(headers)});
    }

    /// Queue a transport-level failure: the plugin receives this status.
    void queue_error(yaddnsc_status status, std::string message) {
        exchanges_.emplace_back(QueuedError{status, std::move(message)});
    }

    /// Build the services table bound to this fake.
    [[nodiscard]] yaddnsc_host_services table() noexcept {
        return yaddnsc_host_services{
                .struct_size = static_cast<uint32_t>(sizeof(yaddnsc_host_services)),
                .api_revision = YADDNSC_DRIVER_API_REVISION,
                .context = this,
                .log = &log_entry,
                .http_exchange = &http_exchange_entry,
                .is_cancelled = &is_cancelled_entry,
        };
    }

private:
    struct QueuedResponse {
        uint32_t status_code;
        std::string body;
        std::vector<std::pair<std::string, std::string>> headers;
    };
    struct QueuedError {
        yaddnsc_status status;
        std::string message;
    };

    static void log_entry(void *context, yaddnsc_log_level level, yaddnsc_string message,
                          const yaddnsc_source_location *location) {
        auto &self = *static_cast<FakeHostServices *>(context);
        self.logs.push_back(LogRecord{
                level,
                std::string(message.data, message.size),
                location != nullptr ? std::string(location->file.data, location->file.size) : std::string{},
                location != nullptr ? location->line : 0,
                location != nullptr ? std::string(location->function.data, location->function.size) : std::string{},
        });
    }

    static yaddnsc_status http_exchange_entry(void *context, const yaddnsc_http_request *request,
                                              yaddnsc_http_response *out_response, yaddnsc_error *out_error) {
        auto &self = *static_cast<FakeHostServices *>(context);

        // Capture (owned copies — the ABI views expire when this call returns).
        CapturedRequest captured;
        captured.url = std::string(request->url.data, request->url.size);
        captured.method = request->method;
        for (size_t i = 0; i < request->header_count; ++i) {
            captured.headers.emplace_back(
                    std::string(request->headers[i].name.data, request->headers[i].name.size),
                    std::string(request->headers[i].value.data, request->headers[i].value.size));
        }
        if (request->body.data != nullptr) {
            captured.body = std::string(reinterpret_cast<const char *>(request->body.data), request->body.size);
        }
        captured.content_type = std::string(request->content_type.data, request->content_type.size);
        self.requests.push_back(std::move(captured));

        if (self.exchanges_.empty()) {
            return write_error(out_error, YADDNSC_STATUS_NETWORK_ERROR, "FakeHostServices: no queued exchange");
        }
        auto queued = std::move(self.exchanges_.front());
        self.exchanges_.pop_front();

        if (const auto *error = std::get_if<QueuedError>(&queued)) {
            return write_error(out_error, error->status, self.arena_copy(error->message));
        }

        const auto &response = std::get<QueuedResponse>(queued);
        out_response->status_code = response.status_code;
        const std::string_view body = self.arena_copy(response.body);
        out_response->body = yaddnsc_bytes{reinterpret_cast<const uint8_t *>(body.data()), body.size()};
        auto &array = self.header_array_arena_.emplace_back();
        for (const auto &[name, value]: response.headers) {
            array.push_back({yaddnsc_string{self.arena_copy(name).data(), name.size()},
                             yaddnsc_string{self.arena_copy(value).data(), value.size()}});
        }
        out_response->headers = array.data();
        out_response->header_count = array.size();
        return YADDNSC_STATUS_OK;
    }

    static int is_cancelled_entry(void *context) noexcept {
        return static_cast<FakeHostServices *>(context)->cancelled ? 1 : 0;
    }

    static yaddnsc_status write_error(yaddnsc_error *out_error, yaddnsc_status status, std::string_view message) {
        if (out_error != nullptr && out_error->struct_size >= YADDNSC_ERROR_MIN_SIZE) {
            out_error->status = status;
            out_error->message = yaddnsc_string{message.data(), message.size()};
        }
        return status;
    }

    std::string_view arena_copy(std::string_view value) {
        return string_arena_.emplace_back(value);
    }

    std::deque<std::variant<QueuedResponse, QueuedError>> exchanges_;
    std::deque<std::string> string_arena_;
    std::deque<std::vector<yaddnsc_http_header>> header_array_arena_;
};

/// The outcome of one create → update → destroy cycle through the ABI.
struct AbiUpdateResult {
    yaddnsc_status create_status = YADDNSC_STATUS_OK;
    yaddnsc_status status = YADDNSC_STATUS_OK;
    std::string error_message;
    uint32_t retry_after_seconds = 0;
};

/// The outcome of one create → validate → destroy cycle through the ABI.
struct AbiValidateResult {
    yaddnsc_status create_status = YADDNSC_STATUS_OK;
    yaddnsc_status status = YADDNSC_STATUS_OK;
    std::string error_message;
};

/// Run one full update cycle through the driver's C entry points.
[[nodiscard]] inline AbiUpdateResult run_abi_update(FakeHostServices &fake, std::string_view driver_param_json,
                                                    std::string_view ip_addr, std::string_view rd_type,
                                                    std::string_view domain, std::string_view subdomain,
                                                    std::string_view fqdn) {
    const auto services = fake.table();

    AbiUpdateResult result{};
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));

    yaddnsc_driver *driver = nullptr;
    result.create_status = yaddnsc_driver_create(&services, &driver, &error);
    if (result.create_status != YADDNSC_STATUS_OK) {
        if (error.message.data != nullptr) {
            result.error_message = std::string(error.message.data, error.message.size);
        }
        return result;
    }

    const yaddnsc_update_request request{
            .struct_size = static_cast<uint32_t>(sizeof(request)),
            .ip_address = {ip_addr.data(), ip_addr.size()},
            .record_type = {rd_type.data(), rd_type.size()},
            .domain = {domain.data(), domain.size()},
            .subdomain = {subdomain.data(), subdomain.size()},
            .fqdn = {fqdn.data(), fqdn.size()},
            .driver_param_json = {reinterpret_cast<const uint8_t *>(driver_param_json.data()),
                                  driver_param_json.size()},
    };

    result.status = yaddnsc_driver_update(driver, &request, &error);
    // Copy error bytes out before destroy, as the production host does.
    if (error.message.data != nullptr) {
        result.error_message = std::string(error.message.data, error.message.size);
    }
    result.retry_after_seconds = error.retry_after_seconds;

    yaddnsc_driver_destroy(driver);
    return result;
}

/// Run one full validate cycle through the driver's C entry points: create an
/// instance, call the OPTIONAL yaddnsc_driver_validate entry, then destroy.
/// Validation is a pure parse check — no HTTP exchange is queued or expected.
[[nodiscard]] inline AbiValidateResult run_abi_validate(FakeHostServices &fake, std::string_view driver_param_json) {
    const auto services = fake.table();

    AbiValidateResult result{};
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));

    yaddnsc_driver *driver = nullptr;
    result.create_status = yaddnsc_driver_create(&services, &driver, &error);
    if (result.create_status != YADDNSC_STATUS_OK) {
        if (error.message.data != nullptr) {
            result.error_message = std::string(error.message.data, error.message.size);
        }
        return result;
    }

    result.status =
            yaddnsc_driver_validate(driver, yaddnsc_string{driver_param_json.data(), driver_param_json.size()}, &error);
    // Copy error bytes out before destroy, as the production host does.
    if (error.message.data != nullptr) {
        result.error_message = std::string(error.message.data, error.message.size);
    }

    yaddnsc_driver_destroy(driver);
    return result;
}

#endif // YADDNSC_TEST_DRIVER_ABI_TEST_HARNESS_H
