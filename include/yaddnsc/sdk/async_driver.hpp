#ifndef YADDNSC_SDK_ASYNC_DRIVER_HPP
#define YADDNSC_SDK_ASYNC_DRIVER_HPP

/// Async plugin SDK (ABI 1.1), source-level only.
///
/// This is a separate interface from the sync Driver/Services/UpdateContext.
/// It provides a lazy Task, one sequential exchange awaitable, and an entry
/// macro that emits only the async execution group. It never exposes host
/// coroutine types, STL objects, or C++ objects across the C ABI.

#include <coroutine>
#include <cstdint>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "yaddnsc/sdk/driver.hpp"

namespace yaddnsc::sdk {

/// An owned async HTTP response, independent of the ABI event and awaitable.
/// Body and header strings preserve their exact bytes, header order, casing
/// and duplicates. Copies own independent storage; moves transfer ownership.
struct AsyncHttpResponse {
    uint32_t status_code = 0;
    std::vector<HttpHeader> headers;
    std::string body;
};

/// Transport/cancellation failures are HttpError; HTTP 4xx/5xx are responses.
using AsyncExchangeResult = std::expected<AsyncHttpResponse, HttpError>;

/* ── Lazy task ────────────────────────────────────────────────────────────*/

/// A lazy, move-only task owned by the async instance. Only the instance
/// resumes it; it is not a general-purpose host runtime.
template<typename T>
class Task {
public:
    struct promise_type;
    using Handle = std::coroutine_handle<promise_type>;

    struct promise_type {
        std::optional<T> value;
        std::exception_ptr error;

        [[nodiscard]] Task get_return_object() noexcept { return Task{Handle::from_promise(*this)}; }
        [[nodiscard]] std::suspend_always initial_suspend() noexcept { return {}; }
        [[nodiscard]] std::suspend_always final_suspend() noexcept { return {}; }
        void return_value(T result) { value.emplace(std::move(result)); }
        void unhandled_exception() noexcept { error = std::current_exception(); }
    };

    Task() noexcept = default;
    explicit Task(Handle handle) noexcept : handle_(handle) {}

    /// A lazy task that completes with @p value when the instance first
    /// resumes it. Useful for an update that rejects the request before any
    /// exchange.
    [[nodiscard]] static Task from_value(T value) { co_return std::move(value); }
    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            destroy();
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    ~Task() { destroy(); }

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(handle_); }
    [[nodiscard]] bool done() const noexcept { return handle_ && handle_.done(); }
    void resume() {
        if (handle_) {
            handle_.resume();
        }
    }

    [[nodiscard]] T take_result() {
        if (handle_.promise().error) {
            std::rethrow_exception(handle_.promise().error);
        }
        if (!handle_.promise().value.has_value()) {
            throw std::logic_error("async task completed without a result");
        }
        return std::move(*handle_.promise().value);
    }

private:
    void destroy() noexcept {
        if (handle_) {
            handle_.destroy();
            handle_ = {};
        }
    }

    Handle handle_{};
};

/* ── Services / update context / awaitable ────────────────────────────────*/

class ExchangeAwaitable;
class AsyncExchangeHost;
class AsyncServices;
class AsyncUpdateContext;

/// The async table callbacks a driver may use. `exchange` is sequential: one
/// pending request per instance, resumed by the host event.
class AsyncServices {
public:
    AsyncServices(const yaddnsc_async_services* services, AsyncExchangeHost* host) noexcept
        : services_(services), host_(host) {}

    [[nodiscard]] const yaddnsc_async_services* get() const noexcept { return services_; }

    [[nodiscard]] bool is_cancelled() const noexcept {
        return services_ != nullptr && services_->is_cancelled != nullptr &&
               services_->is_cancelled(services_->context) != 0;
    }

    [[nodiscard]] int32_t (*log_enabled_fn() const noexcept)(void*, yaddnsc_log_level) {
        return services_ != nullptr ? services_->log_enabled : nullptr;
    }

    /// Await one HTTP exchange. The returned awaitable owns a copy of the
    /// request views before calling http_start.
    [[nodiscard]] ExchangeAwaitable exchange(HttpRequest request) const;

private:
    const yaddnsc_async_services* services_;
    AsyncExchangeHost* host_;
};

/// Everything a driver needs for one async update.
class AsyncUpdateContext {
public:
    AsyncUpdateContext(const UpdateRequest& request, AsyncServices services) noexcept
        : request_(request), services_(services) {}

    [[nodiscard]] const UpdateRequest& request() const noexcept { return request_; }
    [[nodiscard]] AsyncServices services() const noexcept { return services_; }
    [[nodiscard]] bool is_cancelled() const noexcept { return services_.is_cancelled(); }

    [[nodiscard]] ExchangeAwaitable exchange(HttpRequest request) const;

private:
    UpdateRequest request_;
    AsyncServices services_;
};

/// Internal hand-off from an awaitable to its owning instance.
class AsyncExchangeHost {
public:
    virtual ~AsyncExchangeHost() = default;
    /// True: the coroutine is suspended until an event arrives. False: the
    /// awaitable already holds an error and the coroutine may continue.
    [[nodiscard]] virtual bool start_exchange(ExchangeAwaitable& awaitable,
                                              std::coroutine_handle<> handle) = 0;
};

/// One sequential exchange. Owns the request before suspension and copies
/// the borrowed ABI response before resuming business code. await_resume()
/// transfers the owned result; no response views borrow from this awaitable.
class ExchangeAwaitable {
public:
    ExchangeAwaitable(AsyncExchangeHost& host, HttpRequest request)
        : host_(&host), request_(std::move(request)) {}
    ExchangeAwaitable(const ExchangeAwaitable&) = delete;
    ExchangeAwaitable& operator=(const ExchangeAwaitable&) = delete;

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle) {
        try {
            prepare_request();
            return host_->start_exchange(*this, handle);
        } catch (...) {
            if (!error_message_) {
                error_message_ = "async exchange preparation failed";
            }
            result_ = std::unexpected(HttpError{YADDNSC_STATUS_INTERNAL_ERROR, *error_message_, 0});
            return false;
        }
    }

    [[nodiscard]] AsyncExchangeResult await_resume() noexcept { return std::move(result_); }

    [[nodiscard]] const yaddnsc_http_request& c_request() const noexcept { return c_request_; }

    void set_response(const yaddnsc_http_response& response) {
        AsyncHttpResponse value;
        value.status_code = response.status_code;
        value.body = std::string(detail::to_view(response.body));
        value.headers.reserve(response.header_count);
        for (size_t i = 0; i < response.header_count; ++i) {
            value.headers.push_back({std::string(detail::to_view(response.headers[i].name)),
                                     std::string(detail::to_view(response.headers[i].value))});
        }
        result_ = std::move(value);
    }

    void set_error(yaddnsc_status status, const yaddnsc_error& error) {
        error_message_ = std::string(detail::to_view(error.message));
        result_ = std::unexpected(HttpError{status, *error_message_, error.retry_after_seconds});
    }

    void set_cancelled() {
        error_message_ = "the request was cancelled";
        result_ = std::unexpected(HttpError{YADDNSC_STATUS_CANCELLED, *error_message_, 0});
    }

    void set_internal_error(std::string message) {
        error_message_ = std::move(message);
        result_ = std::unexpected(HttpError{YADDNSC_STATUS_INTERNAL_ERROR, *error_message_, 0});
    }

private:
    void prepare_request() {
        url_ = std::string(request_.url);
        content_type_ = std::string(request_.content_type);
        header_storage_.clear();
        c_headers_.clear();
        header_storage_.reserve(request_.headers.size());
        for (const auto& header : request_.headers) {
            header_storage_.emplace_back(std::string(header.name), std::string(header.value));
        }
        c_headers_.reserve(header_storage_.size());
        for (const auto& [name, value] : header_storage_) {
            c_headers_.push_back({{name.data(), name.size()}, {value.data(), value.size()}});
        }
        if (request_.body.has_value()) {
            body_ = *request_.body;
        }
        c_request_ = yaddnsc_http_request{
            .struct_size = static_cast<uint32_t>(sizeof(yaddnsc_http_request)),
            .url = {url_.data(), url_.size()},
            .method = static_cast<yaddnsc_http_method>(request_.method),
            .headers = c_headers_.empty() ? nullptr : c_headers_.data(),
            .header_count = c_headers_.size(),
            .content_type = {content_type_.data(), content_type_.size()},
            .body = request_.body.has_value()
                        ? yaddnsc_bytes{reinterpret_cast<const uint8_t*>(body_.data()), body_.size()}
                        : yaddnsc_bytes{},
        };
    }

    AsyncExchangeHost* host_;
    HttpRequest request_;
    yaddnsc_http_request c_request_{};
    std::string url_;
    std::string content_type_;
    std::string body_;
    std::vector<std::pair<std::string, std::string>> header_storage_;
    std::vector<yaddnsc_http_header> c_headers_;
    AsyncExchangeResult result_{std::unexpect,
                                HttpError{YADDNSC_STATUS_INTERNAL_ERROR, "async exchange not started", 0}};
    std::optional<std::string> error_message_;
};

inline ExchangeAwaitable AsyncServices::exchange(HttpRequest request) const {
    return ExchangeAwaitable{*host_, std::move(request)};
}

inline ExchangeAwaitable AsyncUpdateContext::exchange(HttpRequest request) const {
    return services_.exchange(std::move(request));
}

/* ── Lazy async logging ───────────────────────────────────────────────────*/

namespace detail {

struct AsyncLogTarget {
    const yaddnsc_async_services* services = nullptr;
    int32_t (*log_enabled)(void*, yaddnsc_log_level) = nullptr;
};

[[nodiscard]] inline AsyncLogTarget to_async_log_target(const yaddnsc_async_services* services) noexcept {
    return {services, services != nullptr ? services->log_enabled : nullptr};
}

[[nodiscard]] inline AsyncLogTarget to_async_log_target(const AsyncServices& services) noexcept {
    return {services.get(), services.log_enabled_fn()};
}

[[nodiscard]] inline AsyncLogTarget to_async_log_target(const AsyncUpdateContext& context) noexcept {
    return to_async_log_target(context.services());
}

template<typename... Args>
inline void async_log_message(const yaddnsc_async_services* services, yaddnsc_log_level level,
                              std::string_view file, int32_t line, std::string_view function,
                              std::format_string<Args...> fmt, Args&&... args) noexcept {
    if (services == nullptr || services->log == nullptr) {
        return;
    }
    try {
        const std::string message = fmt::format(fmt, std::forward<Args>(args)...);
        const yaddnsc_source_location location{make_view(file), line, make_view(function)};
        services->log(services->context, level, make_view(message), &location);
    } catch (...) {
    }
}

template<typename GetLevel, typename GetServices, typename Log>
inline void async_log_lazy(GetLevel&& get_level, GetServices&& get_services, Log&& log) noexcept {
    try {
        const AsyncLogTarget target = to_async_log_target(get_services());
        if (target.services == nullptr || target.services->log == nullptr) {
            return;
        }
        const yaddnsc_log_level level = get_level();
        if (target.log_enabled != nullptr) {
            bool enabled = true;
            try {
                enabled = target.log_enabled(target.services->context, level) != 0;
            } catch (...) {
                enabled = true;
            }
            if (!enabled) {
                return;
            }
        }
        log(target.services, level);
    } catch (...) {
    }
}

}  // namespace detail

#define YADDNSC_ASYNC_SDK_LOG(svc, level, ...)                                                                        \
    [&](const char* yaddnsc_log_function) {                                                                           \
        ::yaddnsc::sdk::detail::async_log_lazy(                                                                       \
            [&]() { return static_cast<yaddnsc_log_level>(level); },                                                  \
            [&]() { return (svc); },                                                                                  \
            [&](const yaddnsc_async_services* yaddnsc_log_services, yaddnsc_log_level yaddnsc_log_level_value) {       \
                ::yaddnsc::sdk::detail::async_log_message(yaddnsc_log_services, yaddnsc_log_level_value, __FILE__,      \
                                                          __LINE__, yaddnsc_log_function, __VA_ARGS__);                \
            });                                                                                                       \
    }(__FUNCTION__)
#define YADDNSC_ASYNC_SDK_LOG_TRACE(svc, ...) YADDNSC_ASYNC_SDK_LOG(svc, YADDNSC_LOG_TRACE, __VA_ARGS__)
#define YADDNSC_ASYNC_SDK_LOG_DEBUG(svc, ...) YADDNSC_ASYNC_SDK_LOG(svc, YADDNSC_LOG_DEBUG, __VA_ARGS__)
#define YADDNSC_ASYNC_SDK_LOG_INFO(svc, ...) YADDNSC_ASYNC_SDK_LOG(svc, YADDNSC_LOG_INFO, __VA_ARGS__)
#define YADDNSC_ASYNC_SDK_LOG_WARN(svc, ...) YADDNSC_ASYNC_SDK_LOG(svc, YADDNSC_LOG_WARN, __VA_ARGS__)
#define YADDNSC_ASYNC_SDK_LOG_ERROR(svc, ...) YADDNSC_ASYNC_SDK_LOG(svc, YADDNSC_LOG_ERROR, __VA_ARGS__)

/* ── Driver base ──────────────────────────────────────────────────────────*/

class AsyncDriver : public DriverHelpers {
public:
    virtual ~AsyncDriver() noexcept = default;

    /// Start one update. The returned task is resumed by the async instance;
    /// it may await `AsyncUpdateContext::exchange` sequentially.
    [[nodiscard]] virtual Task<Result> update(AsyncUpdateContext& context) = 0;

    /// Validate driver_param_json without network access.
    [[nodiscard]] virtual Result validate(std::string_view driver_param_json) const = 0;

protected:
    /// Async counterpart of the bundled update tail: log the request, await
    /// the exchange, then translate transport failures and upstream
    /// rejections. A capturing checker may return Result for a detailed error;
    /// the bool checker form keeps the standard status classification.
    template<typename CheckResponse>
    static Task<Result> run_update(AsyncUpdateContext& context, std::string_view driver_name, HttpRequest request,
                                   CheckResponse check_response,
                                   std::source_location location = std::source_location::current()) {
        const auto& params = context.request();
        const auto file = location.file_name();
        const auto line = static_cast<int32_t>(location.line());
        const auto function = location.function_name();

        detail::async_log_lazy(
            [&] { return YADDNSC_LOG_DEBUG; }, [&] { return context.services(); },
            [&](const yaddnsc_async_services* services, yaddnsc_log_level level) {
                detail::async_log_message(services, level, file, line, function,
                                          "Domain \"{}\" ({}) received DNS record update request from driver '{}', {}",
                                          params.fqdn, params.record_type, driver_name, format_request(request));
            });

        auto operation = context.exchange(request);
        auto response = co_await operation;
        if (!response) {
            const auto services = context.services();
            detail::async_log_message(services.get(), YADDNSC_LOG_WARN, file, line, function,
                                      "Domain \"{}\" ({}) update failed: {}", params.fqdn, params.record_type,
                                      response.error().message);
            co_return std::unexpected(
                Error{response.error().status, response.error().message, response.error().retry_after_seconds});
        }

        auto checked = std::invoke(check_response, *response, context.services());
        if (!checked) {
            if constexpr (std::same_as<decltype(checked), Result>) {
                co_return checked;
            }
            const auto failure = detail::classify_upstream_failure(*response);
            const auto services = context.services();
            detail::async_log_message(services.get(), YADDNSC_LOG_WARN, file, line, function,
                                      "Domain \"{}\" ({}) update rejected by upstream", params.fqdn,
                                      params.record_type);
            co_return std::unexpected(Error{failure.status,
                                            fmt::format("Domain {} ({}) update rejected by upstream", params.fqdn,
                                                        params.record_type),
                                            failure.retry_after_seconds});
        }
        co_return {};
    }
};

/* ── Instance / entry machinery ───────────────────────────────────────────*/

namespace detail {

struct OwnedUpdateRequest {
    std::string ip_address;
    std::string record_type;
    std::string domain;
    std::string subdomain;
    std::string fqdn;
    std::string driver_param_json;

    [[nodiscard]] UpdateRequest view() const noexcept {
        return UpdateRequest{
            .ip_address = ip_address,
            .record_type = record_type,
            .domain = domain,
            .subdomain = subdomain,
            .fqdn = fqdn,
            .driver_param_json = driver_param_json,
        };
    }
};

inline bool valid_services(const yaddnsc_async_services* services, uint16_t minimum_minor) noexcept {
    return services != nullptr && services->struct_size >= YADDNSC_ASYNC_SERVICES_MIN_SIZE &&
           services->abi_major == YADDNSC_DRIVER_ABI_MAJOR && services->abi_minor >= minimum_minor &&
           services->context != nullptr && services->log != nullptr && services->is_cancelled != nullptr &&
           services->http_start != nullptr && services->http_cancel != nullptr;
}

inline bool valid_update_request(const yaddnsc_update_request& request) noexcept {
    return request.struct_size >= YADDNSC_UPDATE_REQUEST_MIN_SIZE &&
           yaddnsc_string_is_valid(request.ip_address) && yaddnsc_string_is_valid(request.record_type) &&
           yaddnsc_string_is_valid(request.domain) && yaddnsc_string_is_valid(request.subdomain) &&
           yaddnsc_string_is_valid(request.fqdn) && yaddnsc_bytes_is_valid(request.driver_param_json);
}

template<typename DriverClass>
class AsyncInstance final : public AsyncExchangeHost {
public:
    explicit AsyncInstance(const yaddnsc_async_services* services) : c_services_(services), services_(services, this) {
        if constexpr (std::is_constructible_v<DriverClass, AsyncServices>) {
            driver_ = std::make_unique<DriverClass>(services_);
        } else {
            driver_ = std::make_unique<DriverClass>();
        }
    }

    AsyncInstance(const AsyncInstance&) = delete;
    AsyncInstance& operator=(const AsyncInstance&) = delete;

    [[nodiscard]] yaddnsc_status begin(const yaddnsc_update_request& request, yaddnsc_error* out_error) {
        if (started_ || terminal_ || validated_) {
            write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "async instance already used");
            return YADDNSC_STATUS_INVALID_ARGUMENT;
        }
        if (!valid_update_request(request)) {
            terminal_ = true;
            write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "invalid async update request");
            return YADDNSC_STATUS_INVALID_ARGUMENT;
        }
        try {
            owned_.emplace(OwnedUpdateRequest{
                .ip_address = std::string(to_view(request.ip_address)),
                .record_type = std::string(to_view(request.record_type)),
                .domain = std::string(to_view(request.domain)),
                .subdomain = std::string(to_view(request.subdomain)),
                .fqdn = std::string(to_view(request.fqdn)),
                .driver_param_json = std::string(to_view(request.driver_param_json)),
            });
            context_.emplace(owned_->view(), services_);
            task_ = driver_->update(*context_);
            started_ = true;
            return start_task(out_error);
        } catch (const ConfigParseError& e) {
            terminal_ = true;
            write_error(out_error, YADDNSC_STATUS_INVALID_CONFIG, copy_bounded(e.what(), "invalid configuration"), 0);
            return YADDNSC_STATUS_INVALID_CONFIG;
        } catch (const std::exception& e) {
            terminal_ = true;
            write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR,
                        copy_bounded(e.what(), "unknown exception starting async update"));
            return YADDNSC_STATUS_INTERNAL_ERROR;
        } catch (...) {
            terminal_ = true;
            write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, "unknown exception starting async update");
            return YADDNSC_STATUS_INTERNAL_ERROR;
        }
    }

    [[nodiscard]] yaddnsc_status resume(const yaddnsc_event& event, yaddnsc_error* out_error) {
        if (!started_ || terminal_ || pending_awaitable_ == nullptr) {
            write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "async instance is not waiting for an event");
            return YADDNSC_STATUS_INVALID_ARGUMENT;
        }
        if (event.struct_size < YADDNSC_EVENT_MIN_SIZE || event.abi_major != YADDNSC_DRIVER_ABI_MAJOR ||
            event.abi_minor < 1) {
            write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "invalid async event");
            return YADDNSC_STATUS_INVALID_ARGUMENT;
        }

        if (event.kind == YADDNSC_EVENT_UPDATE_CANCELLED) {
            if (event.op_id != 0 || event.status != YADDNSC_STATUS_CANCELLED || event.response != nullptr ||
                event.error == nullptr || event.error->struct_size < YADDNSC_ERROR_MIN_SIZE ||
                event.error->status != YADDNSC_STATUS_CANCELLED || !yaddnsc_string_is_valid(event.error->message)) {
                write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "invalid async cancellation event");
                return YADDNSC_STATUS_INVALID_ARGUMENT;
            }
            try {
                pending_awaitable_->set_cancelled();
            } catch (...) {
                terminal_ = true;
                write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, "async cancellation copy failed");
                return YADDNSC_STATUS_INTERNAL_ERROR;
            }
        } else if (event.kind == YADDNSC_EVENT_HTTP_FINISHED) {
            if (event.op_id != pending_op_id_ || event.status == YADDNSC_STATUS_PENDING ||
                !yaddnsc_status_is_valid(event.status)) {
                write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "invalid async completion event");
                return YADDNSC_STATUS_INVALID_ARGUMENT;
            }
            if (event.status == YADDNSC_STATUS_OK) {
                if (event.response == nullptr || event.error != nullptr ||
                    event.response->struct_size < YADDNSC_HTTP_RESPONSE_MIN_SIZE ||
                    !has_valid_response(*event.response)) {
                    write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "invalid async response event");
                    return YADDNSC_STATUS_INVALID_ARGUMENT;
                }
                try {
                    pending_awaitable_->set_response(*event.response);
                } catch (...) {
                    terminal_ = true;
                    write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, "async response copy failed");
                    return YADDNSC_STATUS_INTERNAL_ERROR;
                }
            } else {
                if (event.error == nullptr || event.response != nullptr ||
                    event.error->struct_size < YADDNSC_ERROR_MIN_SIZE || event.error->status != event.status ||
                    !yaddnsc_string_is_valid(event.error->message)) {
                    write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "invalid async error event");
                    return YADDNSC_STATUS_INVALID_ARGUMENT;
                }
                try {
                    pending_awaitable_->set_error(event.status, *event.error);
                } catch (...) {
                    terminal_ = true;
                    write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, "async error copy failed");
                    return YADDNSC_STATUS_INTERNAL_ERROR;
                }
            }
        } else {
            write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "unknown async event kind");
            return YADDNSC_STATUS_INVALID_ARGUMENT;
        }

        const std::coroutine_handle<> handle = std::exchange(pending_handle_, {});
        pending_awaitable_ = nullptr;
        pending_op_id_ = 0;
        try {
            handle.resume();
            return settle(out_error);
        } catch (const ConfigParseError& e) {
            terminal_ = true;
            write_error(out_error, YADDNSC_STATUS_INVALID_CONFIG, copy_bounded(e.what(), "invalid configuration"), 0);
            return YADDNSC_STATUS_INVALID_CONFIG;
        } catch (const std::exception& e) {
            terminal_ = true;
            write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, copy_bounded(e.what(), "async task failed"));
            return YADDNSC_STATUS_INTERNAL_ERROR;
        } catch (...) {
            terminal_ = true;
            write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, "async task failed");
            return YADDNSC_STATUS_INTERNAL_ERROR;
        }
    }

    [[nodiscard]] yaddnsc_status validate(yaddnsc_string driver_param_json, yaddnsc_error* out_error) {
        if (!yaddnsc_string_is_valid(driver_param_json)) {
            write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "invalid driver_param_json view");
            return YADDNSC_STATUS_INVALID_ARGUMENT;
        }
        if (started_ || terminal_ || validated_) {
            write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "async instance already used");
            return YADDNSC_STATUS_INVALID_ARGUMENT;
        }
        // validate is the second one-shot operation an instance accepts; mark
        // it before running plugin code so a throw or a failed check still
        // consumes the instance.
        validated_ = true;
        try {
            Result result = driver_->validate(to_view(driver_param_json));
            if (result.has_value()) {
                return YADDNSC_STATUS_OK;
            }
            const Error& error = result.error();
            const yaddnsc_status status = !yaddnsc_status_is_valid(error.status) || error.status == YADDNSC_STATUS_OK
                                              ? YADDNSC_STATUS_INTERNAL_ERROR
                                              : error.status;
            error_storage_ = error.message;
            write_error(out_error, status, error_storage_, error.retry_after_seconds);
            return status;
        } catch (const ConfigParseError& e) {
            write_error(out_error, YADDNSC_STATUS_INVALID_CONFIG, copy_bounded(e.what(), "invalid configuration"), 0);
            return YADDNSC_STATUS_INVALID_CONFIG;
        } catch (const std::exception& e) {
            write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR,
                        copy_bounded(e.what(), "unknown exception during async validate"), 0);
            return YADDNSC_STATUS_INTERNAL_ERROR;
        } catch (...) {
            write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, "unknown exception during async validate", 0);
            return YADDNSC_STATUS_INTERNAL_ERROR;
        }
    }

    [[nodiscard]] bool start_exchange(ExchangeAwaitable& awaitable,
                                      std::coroutine_handle<> handle) override {
        if (pending_awaitable_ != nullptr) {
            awaitable.set_internal_error("another async exchange is already pending");
            return false;
        }
        yaddnsc_error error{};
        error.struct_size = static_cast<uint32_t>(sizeof(error));
        uint64_t op_id = 0;
        const yaddnsc_status status =
            c_services_->http_start(c_services_->context, &awaitable.c_request(), &op_id, &error);
        if (status != YADDNSC_STATUS_OK) {
            awaitable.set_error(status, error);
            return false;
        }
        if (op_id == 0) {
            awaitable.set_internal_error("host returned a zero async op id");
            return false;
        }
        pending_awaitable_ = &awaitable;
        pending_handle_ = handle;
        pending_op_id_ = op_id;
        return true;
    }

private:
    [[nodiscard]] yaddnsc_status start_task(yaddnsc_error* out_error) {
        try {
            task_.resume();
            return settle(out_error);
        } catch (const ConfigParseError& e) {
            terminal_ = true;
            write_error(out_error, YADDNSC_STATUS_INVALID_CONFIG, copy_bounded(e.what(), "invalid configuration"), 0);
            return YADDNSC_STATUS_INVALID_CONFIG;
        } catch (const std::exception& e) {
            terminal_ = true;
            write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, copy_bounded(e.what(), "async task failed"));
            return YADDNSC_STATUS_INTERNAL_ERROR;
        } catch (...) {
            terminal_ = true;
            write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, "async task failed");
            return YADDNSC_STATUS_INTERNAL_ERROR;
        }
    }

    [[nodiscard]] yaddnsc_status settle(yaddnsc_error* out_error) {
        if (pending_awaitable_ != nullptr) {
            return YADDNSC_STATUS_PENDING;
        }
        if (!task_.done()) {
            terminal_ = true;
            write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR,
                        "async task suspended without a pending exchange");
            return YADDNSC_STATUS_INTERNAL_ERROR;
        }
        return finish(out_error);
    }

    [[nodiscard]] yaddnsc_status finish(yaddnsc_error* out_error) {
        terminal_ = true;
        Result result = task_.take_result();
        if (result.has_value()) {
            return YADDNSC_STATUS_OK;
        }
        const Error& error = result.error();
        const yaddnsc_status status = !yaddnsc_status_is_valid(error.status) || error.status == YADDNSC_STATUS_OK ||
                                              error.status == YADDNSC_STATUS_PENDING
                                          ? YADDNSC_STATUS_INTERNAL_ERROR
                                          : error.status;
        error_storage_ = error.message;
        write_error(out_error, status, error_storage_, error.retry_after_seconds);
        return status;
    }

    static void write_error(yaddnsc_error* out_error, yaddnsc_status status, std::string_view message,
                            uint32_t retry_after_seconds = 0) noexcept {
        yaddnsc::sdk::detail::write_error(out_error, status, message, retry_after_seconds);
    }

    const yaddnsc_async_services* c_services_;
    AsyncServices services_;
    std::unique_ptr<DriverClass> driver_;
    std::optional<OwnedUpdateRequest> owned_;
    std::optional<AsyncUpdateContext> context_;
    Task<Result> task_;
    ExchangeAwaitable* pending_awaitable_ = nullptr;
    std::coroutine_handle<> pending_handle_{};
    uint64_t pending_op_id_ = 0;
    bool started_ = false;
    bool terminal_ = false;
    bool validated_ = false;
    std::string error_storage_;
};

template<typename DriverClass, uint16_t MinimumMinor>
inline yaddnsc_status create_async_driver(const yaddnsc_async_services* services, yaddnsc_async_driver** out_driver,
                                          yaddnsc_error* out_error) noexcept {
    if (services == nullptr || out_driver == nullptr) {
        yaddnsc::sdk::detail::write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT,
                                          "services and out_driver must not be null", 0);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    *out_driver = nullptr;
    if (!valid_services(services, MinimumMinor)) {
        yaddnsc::sdk::detail::write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT,
                                          "incompatible async host services table", 0);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    static_assert(std::is_nothrow_destructible_v<DriverClass>,
                  "async driver classes must be nothrow destructible across the C ABI");
    try {
        auto instance = std::make_unique<AsyncInstance<DriverClass>>(services);
        *out_driver = reinterpret_cast<yaddnsc_async_driver*>(instance.release());  // NOLINT
        return YADDNSC_STATUS_OK;
    } catch (const std::exception& e) {
        yaddnsc::sdk::detail::write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR,
                                          copy_bounded(e.what(), "unknown exception during async construction"), 0);
    } catch (...) {
        yaddnsc::sdk::detail::write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR,
                                          "unknown exception during async construction", 0);
    }
    return YADDNSC_STATUS_INTERNAL_ERROR;
}

}  // namespace detail
}  // namespace yaddnsc::sdk

/* ── Driver definition macro ──────────────────────────────────────────────*/

/// Emit get_descriptor plus the complete async execution group. The default
/// minimum host ABI is 1.1; use the _MIN form only when a higher minimum is a
/// real requirement.
#define YADDNSC_DEFINE_ASYNC_DRIVER(DriverClass, driver_name, driver_description, driver_author, driver_version,      \
                                    driver_capabilities)                                                             \
    YADDNSC_DEFINE_ASYNC_DRIVER_MIN(DriverClass, driver_name, driver_description, driver_author, driver_version,       \
                                    driver_capabilities, 1)

#define YADDNSC_DEFINE_ASYNC_DRIVER_MIN(DriverClass, driver_name, driver_description, driver_author, driver_version,     \
                                        driver_capabilities, minimum_abi_minor)                                         \
    namespace {                                                                                                         \
    static_assert((minimum_abi_minor) >= 1 && (minimum_abi_minor) <= YADDNSC_DRIVER_ABI_MINOR);                         \
    constexpr yaddnsc_driver_descriptor YADDNSC_ASYNC_SDK_DESCRIPTOR = {                                                \
        .struct_size = sizeof(yaddnsc_driver_descriptor),                                                               \
        .abi_major = YADDNSC_DRIVER_ABI_MAJOR,                                                                          \
        .abi_minor = (minimum_abi_minor),                                                                               \
        .magic = YADDNSC_DRIVER_MAGIC,                                                                                  \
        .name = ::yaddnsc::sdk::detail::descriptor_string(driver_name),                                                 \
        .version = ::yaddnsc::sdk::detail::descriptor_string(driver_version),                                           \
        .author = ::yaddnsc::sdk::detail::descriptor_string(driver_author),                                             \
        .description = ::yaddnsc::sdk::detail::descriptor_string(driver_description),                                    \
        .capabilities = (driver_capabilities),                                                                          \
    };                                                                                                                  \
    }                                                                                                                   \
                                                                                                                        \
    extern "C" YADDNSC_SDK_EXPORT yaddnsc_status yaddnsc_driver_get_descriptor(                                         \
        const yaddnsc_driver_descriptor** out_descriptor) {                                                             \
        if (out_descriptor == nullptr) {                                                                                \
            return YADDNSC_STATUS_INVALID_ARGUMENT;                                                                     \
        }                                                                                                               \
        *out_descriptor = &YADDNSC_ASYNC_SDK_DESCRIPTOR;                                                                \
        return YADDNSC_STATUS_OK;                                                                                       \
    }                                                                                                                   \
                                                                                                                        \
    extern "C" YADDNSC_SDK_EXPORT yaddnsc_status yaddnsc_driver_async_create(                                           \
        const yaddnsc_async_services* services, yaddnsc_async_driver** out_driver, yaddnsc_error* out_error) {          \
        return ::yaddnsc::sdk::detail::create_async_driver<DriverClass, (minimum_abi_minor)>(services, out_driver,       \
                                                                                             out_error);                \
    }                                                                                                                   \
                                                                                                                        \
    extern "C" YADDNSC_SDK_EXPORT void yaddnsc_driver_async_destroy(yaddnsc_async_driver* driver) {                     \
        delete reinterpret_cast<::yaddnsc::sdk::detail::AsyncInstance<DriverClass>*>(driver);  /* NOLINT */              \
    }                                                                                                                   \
                                                                                                                        \
    extern "C" YADDNSC_SDK_EXPORT yaddnsc_status yaddnsc_driver_update_async_begin(                                     \
        yaddnsc_async_driver* driver, const yaddnsc_update_request* request, yaddnsc_error* out_error) {                \
        if (driver == nullptr || request == nullptr) {                                                                  \
            ::yaddnsc::sdk::detail::write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT,                             \
                                                "driver and request must not be null", 0);                              \
            return YADDNSC_STATUS_INVALID_ARGUMENT;                                                                     \
        }                                                                                                               \
        return reinterpret_cast<::yaddnsc::sdk::detail::AsyncInstance<DriverClass>*>(driver)->begin(*request,             \
                                                                                                out_error);             \
    }                                                                                                                   \
                                                                                                                        \
    extern "C" YADDNSC_SDK_EXPORT yaddnsc_status yaddnsc_driver_update_async_resume(                                    \
        yaddnsc_async_driver* driver, const yaddnsc_event* event, yaddnsc_error* out_error) {                           \
        if (driver == nullptr || event == nullptr) {                                                                    \
            ::yaddnsc::sdk::detail::write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT,                             \
                                                "driver and event must not be null", 0);                                \
            return YADDNSC_STATUS_INVALID_ARGUMENT;                                                                     \
        }                                                                                                               \
        return reinterpret_cast<::yaddnsc::sdk::detail::AsyncInstance<DriverClass>*>(driver)->resume(*event, out_error); \
    }                                                                                                                   \
                                                                                                                        \
    extern "C" YADDNSC_SDK_EXPORT yaddnsc_status yaddnsc_driver_async_validate(                                         \
        yaddnsc_async_driver* driver, yaddnsc_string driver_param_json, yaddnsc_error* out_error) {                     \
        if (driver == nullptr) {                                                                                        \
            ::yaddnsc::sdk::detail::write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "driver must not be null",   \
                                                0);                                                                     \
            return YADDNSC_STATUS_INVALID_ARGUMENT;                                                                     \
        }                                                                                                               \
        return reinterpret_cast<::yaddnsc::sdk::detail::AsyncInstance<DriverClass>*>(driver)->validate(                 \
            driver_param_json, out_error);                                                                              \
    }

#endif  // YADDNSC_SDK_ASYNC_DRIVER_HPP
