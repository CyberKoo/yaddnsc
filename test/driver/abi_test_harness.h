#ifndef YADDNSC_TEST_DRIVER_ABI_TEST_HARNESS_H
#define YADDNSC_TEST_DRIVER_ABI_TEST_HARNESS_H

/// Shared async harness for driver tests on the v1.1 ABI.
///
/// FakeHostServices implements the async host services table in-process: it
/// captures every accepted request (owned copies), replays queued
/// responses/transport errors as completion events, and records log records
/// with source location. run_abi_update() drives one full async
/// create → begin → resume… → destroy cycle through the C entry points,
/// exactly as the production host does.

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
        std::optional<std::string> body;  ///< nullopt = no body at all
        std::string content_type;

        /// First value of the named header, or nullopt when absent.
        [[nodiscard]] std::optional<std::string> header(std::string_view name) const {
            for (const auto& [n, v] : headers) {
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

    /// Build the async services table bound to this fake.
    [[nodiscard]] yaddnsc_async_services async_table() noexcept {
        return yaddnsc_async_services{
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

    /// The op id most recently accepted by http_start.
    [[nodiscard]] uint64_t last_op_id() const noexcept { return last_op_id_; }

    /// Pop the next queued exchange into the stable event slot. With no queued
    /// exchange the plugin receives a NETWORK_ERROR completion, mirroring the
    /// old synchronous fake's behaviour.
    void make_event(uint64_t op_id) {
        event_ = yaddnsc_event{
            .struct_size = static_cast<uint32_t>(sizeof(yaddnsc_event)),
            .abi_major = YADDNSC_DRIVER_ABI_MAJOR,
            .abi_minor = YADDNSC_DRIVER_ABI_MINOR,
            .kind = YADDNSC_EVENT_HTTP_FINISHED,
            .op_id = op_id,
            .status = YADDNSC_STATUS_OK,
            .response = nullptr,
            .error = nullptr,
        };
        response_ = {};
        error_ = {};

        if (exchanges_.empty()) {
            set_error(YADDNSC_STATUS_NETWORK_ERROR, "FakeHostServices: no queued exchange");
            return;
        }
        auto queued = std::move(exchanges_.front());
        exchanges_.pop_front();

        if (const auto* queued_error = std::get_if<QueuedError>(&queued)) {
            set_error(queued_error->status, queued_error->message);
            return;
        }

        const auto& queued_response = std::get<QueuedResponse>(queued);
        response_.struct_size = static_cast<uint32_t>(sizeof(yaddnsc_http_response));
        response_.status_code = queued_response.status_code;
        const std::string_view body = arena_copy(queued_response.body);
        response_.body = yaddnsc_bytes{reinterpret_cast<const uint8_t*>(body.data()), body.size()};
        auto& array = header_array_arena_.emplace_back();
        for (const auto& [name, value] : queued_response.headers) {
            array.push_back({yaddnsc_string{arena_copy(name).data(), name.size()},
                             yaddnsc_string{arena_copy(value).data(), value.size()}});
        }
        response_.headers = array.data();
        response_.header_count = array.size();
        event_.status = YADDNSC_STATUS_OK;
        event_.response = &response_;
        event_.error = nullptr;
    }

    [[nodiscard]] const yaddnsc_event& event() const noexcept { return event_; }

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

    void set_error(yaddnsc_status status, std::string_view message) {
        error_.struct_size = static_cast<uint32_t>(sizeof(yaddnsc_error));
        error_.status = status;
        error_.retry_after_seconds = 0;
        error_.message = yaddnsc_string{arena_copy(message).data(), message.size()};
        event_.status = status;
        event_.response = nullptr;
        event_.error = &error_;
    }

    static void log_entry(void* context, yaddnsc_log_level level, yaddnsc_string message,
                          const yaddnsc_source_location* location) {
        auto& self = *static_cast<FakeHostServices*>(context);
        self.logs.push_back(LogRecord{
            level,
            std::string(message.data, message.size),
            location != nullptr ? std::string(location->file.data, location->file.size) : std::string{},
            location != nullptr ? location->line : 0,
            location != nullptr ? std::string(location->function.data, location->function.size) : std::string{},
        });
    }

    static std::int32_t log_enabled_entry(void*, yaddnsc_log_level) noexcept { return 1; }

    static yaddnsc_status http_start_entry(void* context, const yaddnsc_http_request* request,
                                           uint64_t* out_op_id, yaddnsc_error* out_error) {
        auto& self = *static_cast<FakeHostServices*>(context);
        if (request == nullptr || out_op_id == nullptr) {
            return write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "request and out_op_id must not be null");
        }

        CapturedRequest captured;
        captured.url = std::string(request->url.data, request->url.size);
        captured.method = request->method;
        for (size_t i = 0; i < request->header_count; ++i) {
            captured.headers.emplace_back(std::string(request->headers[i].name.data, request->headers[i].name.size),
                                          std::string(request->headers[i].value.data, request->headers[i].value.size));
        }
        if (request->body.data != nullptr) {
            captured.body = std::string(reinterpret_cast<const char*>(request->body.data), request->body.size);
        }
        captured.content_type = std::string(request->content_type.data, request->content_type.size);
        self.requests.push_back(std::move(captured));

        self.last_op_id_ = ++self.next_op_id_;
        *out_op_id = self.last_op_id_;
        return YADDNSC_STATUS_OK;
    }

    static yaddnsc_status http_cancel_entry(void*, uint64_t, yaddnsc_error*) { return YADDNSC_STATUS_OK; }

    static int32_t is_cancelled_entry(void* context) noexcept {
        return static_cast<FakeHostServices*>(context)->cancelled ? 1 : 0;
    }

    static yaddnsc_status write_error(yaddnsc_error* out_error, yaddnsc_status status, std::string_view message) {
        if (out_error != nullptr && out_error->struct_size >= YADDNSC_ERROR_MIN_SIZE) {
            out_error->status = status;
            out_error->message = yaddnsc_string{message.data(), message.size()};
        }
        return status;
    }

    std::string_view arena_copy(std::string_view value) { return string_arena_.emplace_back(value); }

    std::deque<std::variant<QueuedResponse, QueuedError>> exchanges_;
    std::deque<std::string> string_arena_;
    std::deque<std::vector<yaddnsc_http_header>> header_array_arena_;
    yaddnsc_event event_{};
    yaddnsc_http_response response_{};
    yaddnsc_error error_{};
    uint64_t next_op_id_ = 0;
    uint64_t last_op_id_ = 0;
};

/// The outcome of one async create → begin/resume… → destroy cycle.
struct AbiUpdateResult {
    yaddnsc_status create_status = YADDNSC_STATUS_OK;
    yaddnsc_status status = YADDNSC_STATUS_OK;
    std::string error_message;
    uint32_t retry_after_seconds = 0;
};

/// The outcome of one async create → validate → destroy cycle.
struct AbiValidateResult {
    yaddnsc_status create_status = YADDNSC_STATUS_OK;
    yaddnsc_status status = YADDNSC_STATUS_OK;
    std::string error_message;
};

/// Run one full async update cycle through the driver's C entry points.
[[nodiscard]] inline AbiUpdateResult run_abi_update(FakeHostServices& fake, std::string_view driver_param_json,
                                                    std::string_view ip_addr, std::string_view rd_type,
                                                    std::string_view domain, std::string_view subdomain,
                                                    std::string_view fqdn) {
    const auto services = fake.async_table();

    AbiUpdateResult result{};
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));

    yaddnsc_async_driver* driver = nullptr;
    result.create_status = yaddnsc_driver_async_create(&services, &driver, &error);
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
        .driver_param_json = {reinterpret_cast<const uint8_t*>(driver_param_json.data()), driver_param_json.size()},
    };

    result.status = yaddnsc_driver_update_async_begin(driver, &request, &error);
    int resumes = 0;
    while (result.status == YADDNSC_STATUS_PENDING) {
        if (++resumes > 8) {
            result.status = YADDNSC_STATUS_INTERNAL_ERROR;
            result.error_message = "FakeHostServices: too many pending resumes";
            break;
        }
        fake.make_event(fake.last_op_id());
        result.status = yaddnsc_driver_update_async_resume(driver, &fake.event(), &error);
    }

    // Copy error bytes out before destroy, as the production host does.
    if (error.message.data != nullptr) {
        result.error_message = std::string(error.message.data, error.message.size);
    }
    result.retry_after_seconds = error.retry_after_seconds;

    yaddnsc_driver_async_destroy(driver);
    return result;
}

/// Run one full async validate cycle: create an instance, call the optional
/// async validate entry, then destroy. Validation is a pure parse check.
[[nodiscard]] inline AbiValidateResult run_abi_validate(FakeHostServices& fake, std::string_view driver_param_json) {
    const auto services = fake.async_table();

    AbiValidateResult result{};
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));

    yaddnsc_async_driver* driver = nullptr;
    result.create_status = yaddnsc_driver_async_create(&services, &driver, &error);
    if (result.create_status != YADDNSC_STATUS_OK) {
        if (error.message.data != nullptr) {
            result.error_message = std::string(error.message.data, error.message.size);
        }
        return result;
    }

    result.status = yaddnsc_driver_async_validate(
        driver, yaddnsc_string{driver_param_json.data(), driver_param_json.size()}, &error);
    // Copy error bytes out before destroy, as the production host does.
    if (error.message.data != nullptr) {
        result.error_message = std::string(error.message.data, error.message.size);
    }

    yaddnsc_driver_async_destroy(driver);
    return result;
}

#endif  // YADDNSC_TEST_DRIVER_ABI_TEST_HARNESS_H
