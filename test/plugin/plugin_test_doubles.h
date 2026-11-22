#ifndef YADDNSC_TEST_PLUGIN_PLUGIN_TEST_DOUBLES_H
#define YADDNSC_TEST_PLUGIN_PLUGIN_TEST_DOUBLES_H

/// Shared test doubles for the v1 plugin contract tests.
///
/// A scripted host-services table (log / http_exchange / is_cancelled) that
/// mirrors the ABI's memory and struct_size rules, a recording logger, and
/// run_sync_cycle(), which drives one create → update → destroy cycle
/// through a dlopen'ed sync package.
///
/// The table is test-only scaffolding: the production host-services table is
/// the coroutine one (src/infrastructure/plugin/sync/host_services.h).

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <yaddnsc/sdk/driver_abi.h>

#include "coro/host.hpp"
#include "infrastructure/plugin/async/package.h"
#include "infrastructure/plugin/plugin_error.h"
#include "infrastructure/plugin/plugin_loader.h"
#include "infrastructure/plugin/plugin_package.h"
#include "infrastructure/plugin/sync/bridge.h"
#include "infrastructure/plugin/sync/package.h"
#include "mocks/recording_logger.h"

namespace test {

/// Load one driver image and select its protocol package.
///
/// The loader picks the protocol from the exported entry points; a cast that
/// fails means the host selected a different protocol than the fixture
/// declares, so it is reported as a contract violation instead of a null
/// package the caller would dereference.
[[nodiscard]] inline std::expected<std::shared_ptr<const plugin::SyncPluginPackage>, plugin::PluginError>
load_sync_package(const std::string& path) {
    auto loaded = PluginLoader::load(path);
    if (!loaded) {
        return std::unexpected(plugin::PluginError{loaded.error()});
    }
    auto package = std::dynamic_pointer_cast<const plugin::SyncPluginPackage>(*loaded);
    if (package == nullptr) {
        return std::unexpected(plugin::PluginError{plugin::PluginError::Code::CONTRACT_VIOLATION,
                                                   "the loader did not select the synchronous protocol package"});
    }
    return package;
}

/// Load one driver image and select its asynchronous protocol package.
[[nodiscard]] inline std::expected<std::shared_ptr<const plugin::AsyncPluginPackage>, plugin::PluginError>
load_async_package(const std::string& path) {
    auto loaded = PluginLoader::load(path);
    if (!loaded) {
        return std::unexpected(plugin::PluginError{loaded.error()});
    }
    auto package = std::dynamic_pointer_cast<const plugin::AsyncPluginPackage>(*loaded);
    if (package == nullptr) {
        return std::unexpected(plugin::PluginError{plugin::PluginError::Code::CONTRACT_VIOLATION,
                                                   "the loader did not select the asynchronous protocol package"});
    }
    return package;
}

/// A stand-in for one protocol session's bridge registration state.
///
/// The production session owns this state privately; the contract tests need
/// a session they can cancel and inspect, so this double implements the
/// narrow plugin::BridgeSession interface the bridge itself consumes. Its
/// live set mirrors the production one: weak references, so registering a
/// call never keeps it alive.
class TestBridgeSession final : public plugin::BridgeSession {
public:
    TestBridgeSession() = default;

    explicit TestBridgeSession(coro::NurseryToken lifetime) : lifetime_(std::move(lifetime)) {}

    [[nodiscard]] coro::NurseryToken admission_token() const override { return lifetime_; }

    [[nodiscard]] bool cancelled() const noexcept override { return cancelled_.load(std::memory_order_acquire); }

    void register_call(const std::shared_ptr<plugin::BridgeCall>& call) override { calls_.push_back(call); }

    void unregister_call(const std::shared_ptr<plugin::BridgeCall>& call) noexcept override {
        std::erase_if(calls_, [&call](const auto& tracked) { return tracked.lock() == call; });
    }

    /// Latch the cancellation the session's owner would request.
    void request_cancel() noexcept { cancelled_.store(true, std::memory_order_release); }

    /// Bind the nursery the bridge admits calls into.
    void set_admission_token(coro::NurseryToken lifetime) { lifetime_ = std::move(lifetime); }

    /// The calls currently registered with this session and still alive.
    [[nodiscard]] std::size_t live_call_count() const noexcept {
        return static_cast<std::size_t>(
            std::count_if(calls_.begin(), calls_.end(), [](const auto& tracked) { return !tracked.expired(); }));
    }

    [[nodiscard]] bool has_live_call(const plugin::BridgeCall* call) const noexcept {
        return std::any_of(calls_.begin(), calls_.end(), [call](const auto& tracked) {
            const auto live = tracked.lock();
            return live != nullptr && live.get() == call;
        });
    }

private:
    coro::NurseryToken lifetime_;
    std::atomic<bool> cancelled_{false};
    std::vector<std::weak_ptr<plugin::BridgeCall>> calls_;
};

}  // namespace test

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

/// The host-services table used by the ABI contract tests. It mirrors the ABI
/// memory rules: every view a plugin reads points into the arena and stays
/// valid until this object dies.
class TestHostServices {
public:
    TestHostServices(ExplicitRecordingLogger& logger, ScriptedHttpTransport& transport)
        : logger_(logger), transport_(transport) {}

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
        const app::LogLevel mapped = level == YADDNSC_LOG_TRACE   ? app::LogLevel::TRACE
                                     : level == YADDNSC_LOG_DEBUG ? app::LogLevel::DEBUG
                                     : level == YADDNSC_LOG_WARN  ? app::LogLevel::WARN
                                     : level == YADDNSC_LOG_ERROR ? app::LogLevel::ERROR
                                                                  : app::LogLevel::INFO;
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

    ExplicitRecordingLogger& logger_;
    ScriptedHttpTransport& transport_;
    std::deque<std::string> string_arena_;
    std::deque<std::vector<yaddnsc_http_header>> header_array_arena_;
};

/// One per-update host-services context plus its scripted transport.
struct HostUpdateContext {
    ExplicitRecordingLogger logger;
    ScriptedHttpTransport client;
    TestHostServices context;

    HostUpdateContext() : context(logger, client) {}

    [[nodiscard]] yaddnsc_host_services services() noexcept { return context.make_services(); }
};

/// Build a fully-populated update request (all views borrow the arguments).
[[nodiscard]] inline yaddnsc_update_request make_update_request(std::string_view ip_address,
                                                                std::string_view record_type, std::string_view domain,
                                                                std::string_view subdomain, std::string_view fqdn,
                                                                std::string_view driver_param_json) {
    return yaddnsc_update_request{
        .struct_size = static_cast<uint32_t>(sizeof(yaddnsc_update_request)),
        .ip_address = {ip_address.data(), ip_address.size()},
        .record_type = {record_type.data(), record_type.size()},
        .domain = {domain.data(), domain.size()},
        .subdomain = {subdomain.data(), subdomain.size()},
        .fqdn = {fqdn.data(), fqdn.size()},
        .driver_param_json = {reinterpret_cast<const uint8_t*>(driver_param_json.data()), driver_param_json.size()},
    };
}

/// The outcome of one create → update → destroy cycle through a sync package.
struct SyncCycleResult {
    yaddnsc_status create_status = YADDNSC_STATUS_OK;
    yaddnsc_status update_status = YADDNSC_STATUS_OK;
    std::string error_message;
    uint32_t retry_after_seconds = 0;
};

/// Drive one full update cycle through a package's C entry points, copying
/// the error report out synchronously before destroy.
[[nodiscard]] inline SyncCycleResult run_sync_cycle(
    const plugin::SyncPluginPackage& package, const yaddnsc_host_services& services, std::string_view driver_param_json,
    std::string_view ip_address = "192.0.2.1", std::string_view record_type = "A",
    std::string_view domain = "example.com", std::string_view subdomain = "www",
    std::string_view fqdn = "www.example.com") {
    SyncCycleResult result;
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));

    yaddnsc_driver* handle = nullptr;
    result.create_status = package.create(services, &handle, error);
    if (result.create_status != YADDNSC_STATUS_OK) {
        if (error.message.data != nullptr) {
            result.error_message = std::string(error.message.data, error.message.size);
        }
        return result;
    }

    const auto request = make_update_request(ip_address, record_type, domain, subdomain, fqdn, driver_param_json);
    result.update_status = package.update(handle, request, error);
    if (error.message.data != nullptr) {
        result.error_message = std::string(error.message.data, error.message.size);
    }
    result.retry_after_seconds = error.retry_after_seconds;

    package.destroy(handle);
    return result;
}

#endif  // YADDNSC_TEST_PLUGIN_PLUGIN_TEST_DOUBLES_H
