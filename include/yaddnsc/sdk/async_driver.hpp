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
#include <functional>
#include <memory>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <concepts>

#include <yaddnsc/sdk/driver_common.hpp>
#include <yaddnsc/sdk/log.hpp>
#include <yaddnsc/sdk/run_update.hpp>

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
/// Internal hand-off from an awaitable to its owning instance. Host-side only:
/// the class and the awaitable mutators are defined in
/// <yaddnsc/sdk/detail/async_instance.hpp>, which this header includes last.
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

/// One sequential exchange. Owns the request before suspension and copies
/// the borrowed ABI response before resuming business code. await_resume()
/// transfers the owned result; no response views borrow from this awaitable.
///
/// await_suspend and the mutators below hand control to the owning instance
/// and are therefore defined with that host type in
/// <yaddnsc/sdk/detail/async_instance.hpp>; a driver only ever awaits this
/// object.
class ExchangeAwaitable {
public:
    ExchangeAwaitable(AsyncExchangeHost& host, HttpRequest request) : host_(&host), request_(std::move(request)) {}

    ExchangeAwaitable(const ExchangeAwaitable&) = delete;
    ExchangeAwaitable& operator=(const ExchangeAwaitable&) = delete;

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle);

    [[nodiscard]] AsyncExchangeResult await_resume() noexcept { return std::move(result_); }

    /* ── Host-only ABI wiring; not for driver authors ───────────────────────*/

    [[nodiscard]] const yaddnsc_http_request& c_request() const noexcept;

    void set_response(const yaddnsc_http_response& response);

    void set_error(yaddnsc_status status, const yaddnsc_error& error);

    void set_cancelled();

    void set_internal_error(std::string message);

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

        detail::async_log_lazy([&] { return YADDNSC_LOG_DEBUG; }, [&] { return context.services(); },
                               [&](const yaddnsc_async_services* services, yaddnsc_log_level level) {
                                   detail::async_log_message(
                                       services, level, file, line, function,
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
            const auto services = context.services();
            // Every checker form logs the rejection here, so the domain
            // context stays in the log next to the provider detail the
            // driver reported.
            detail::async_log_message(services.get(), YADDNSC_LOG_WARN, file, line, function,
                                      "Domain \"{}\" ({}) update rejected by upstream", params.fqdn,
                                      params.record_type);
            if constexpr (std::same_as<decltype(checked), Result>) {
                co_return checked;
            }
            const auto failure = detail::classify_upstream_failure(*response);
            co_return std::unexpected(
                Error{failure.status,
                      fmt::format("Domain {} ({}) update rejected by upstream", params.fqdn, params.record_type),
                      failure.retry_after_seconds});
        }
        co_return {};
    }
};

}  // namespace yaddnsc::sdk

/* ── Driver definition macro ──────────────────────────────────────────────*/

/// Emit get_descriptor plus the complete async execution group. The default
/// minimum host ABI is 1.1; use the _MIN form only when a higher minimum is a
/// real requirement.
#define YADDNSC_DEFINE_ASYNC_DRIVER(DriverClass, driver_name, driver_description, driver_author, driver_version, \
                                    driver_capabilities)                                                         \
    YADDNSC_DEFINE_ASYNC_DRIVER_MIN(DriverClass, driver_name, driver_description, driver_author, driver_version, \
                                    driver_capabilities, 1)

#define YADDNSC_DEFINE_ASYNC_DRIVER_MIN(DriverClass, driver_name, driver_description, driver_author, driver_version,   \
                                        driver_capabilities, minimum_abi_minor)                                        \
    namespace {                                                                                                        \
    static_assert((minimum_abi_minor) >= 1 && (minimum_abi_minor) <= YADDNSC_DRIVER_ABI_MINOR);                        \
    constexpr yaddnsc_driver_descriptor YADDNSC_ASYNC_SDK_DESCRIPTOR = {                                               \
        .struct_size = sizeof(yaddnsc_driver_descriptor),                                                              \
        .abi_major = YADDNSC_DRIVER_ABI_MAJOR,                                                                         \
        .abi_minor = (minimum_abi_minor),                                                                              \
        .magic = YADDNSC_DRIVER_MAGIC,                                                                                 \
        .name = ::yaddnsc::sdk::detail::descriptor_string(driver_name),                                                \
        .version = ::yaddnsc::sdk::detail::descriptor_string(driver_version),                                          \
        .author = ::yaddnsc::sdk::detail::descriptor_string(driver_author),                                            \
        .description = ::yaddnsc::sdk::detail::descriptor_string(driver_description),                                  \
        .capabilities = (driver_capabilities),                                                                         \
    };                                                                                                                 \
    }                                                                                                                  \
                                                                                                                       \
    extern "C" YADDNSC_SDK_EXPORT yaddnsc_status yaddnsc_driver_get_descriptor(                                        \
        const yaddnsc_driver_descriptor** out_descriptor) {                                                            \
        if (out_descriptor == nullptr) {                                                                               \
            return YADDNSC_STATUS_INVALID_ARGUMENT;                                                                    \
        }                                                                                                              \
        *out_descriptor = &YADDNSC_ASYNC_SDK_DESCRIPTOR;                                                               \
        return YADDNSC_STATUS_OK;                                                                                      \
    }                                                                                                                  \
                                                                                                                       \
    extern "C" YADDNSC_SDK_EXPORT yaddnsc_status yaddnsc_driver_async_create(                                          \
        const yaddnsc_async_services* services, yaddnsc_async_driver** out_driver, yaddnsc_error* out_error) {         \
        return ::yaddnsc::sdk::detail::create_async_driver<DriverClass, (minimum_abi_minor)>(services, out_driver,     \
                                                                                             out_error);               \
    }                                                                                                                  \
                                                                                                                       \
    extern "C" YADDNSC_SDK_EXPORT void yaddnsc_driver_async_destroy(yaddnsc_async_driver* driver) {                    \
        delete reinterpret_cast<::yaddnsc::sdk::detail::AsyncInstance<DriverClass>*>(driver); /* NOLINT */             \
    }                                                                                                                  \
                                                                                                                       \
    extern "C" YADDNSC_SDK_EXPORT yaddnsc_status yaddnsc_driver_update_async_begin(                                    \
        yaddnsc_async_driver* driver, const yaddnsc_update_request* request, yaddnsc_error* out_error) {               \
        if (driver == nullptr || request == nullptr) {                                                                 \
            ::yaddnsc::sdk::detail::write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT,                            \
                                                "driver and request must not be null", 0);                             \
            return YADDNSC_STATUS_INVALID_ARGUMENT;                                                                    \
        }                                                                                                              \
        return reinterpret_cast<::yaddnsc::sdk::detail::AsyncInstance<DriverClass>*>(driver)->begin(*request,          \
                                                                                                    out_error);        \
    }                                                                                                                  \
                                                                                                                       \
    extern "C" YADDNSC_SDK_EXPORT yaddnsc_status yaddnsc_driver_update_async_resume(                                   \
        yaddnsc_async_driver* driver, const yaddnsc_event* event, yaddnsc_error* out_error) {                          \
        if (driver == nullptr || event == nullptr) {                                                                   \
            ::yaddnsc::sdk::detail::write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT,                            \
                                                "driver and event must not be null", 0);                               \
            return YADDNSC_STATUS_INVALID_ARGUMENT;                                                                    \
        }                                                                                                              \
        return reinterpret_cast<::yaddnsc::sdk::detail::AsyncInstance<DriverClass>*>(driver)->resume(*event,           \
                                                                                                     out_error);       \
    }                                                                                                                  \
                                                                                                                       \
    extern "C" YADDNSC_SDK_EXPORT yaddnsc_status yaddnsc_driver_async_validate(                                        \
        yaddnsc_async_driver* driver, yaddnsc_string driver_param_json, yaddnsc_error* out_error) {                    \
        if (driver == nullptr) {                                                                                       \
            ::yaddnsc::sdk::detail::write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "driver must not be null", \
                                                0);                                                                    \
            return YADDNSC_STATUS_INVALID_ARGUMENT;                                                                    \
        }                                                                                                              \
        return reinterpret_cast<::yaddnsc::sdk::detail::AsyncInstance<DriverClass>*>(driver)->validate(                \
            driver_param_json, out_error);                                                                             \
    }

/// The host-side seam and instance machinery the macro above expands to;
/// not part of the interface a driver author writes against. Included last so
/// that including <yaddnsc/sdk/async_driver.hpp> alone still resolves it.
#include <yaddnsc/sdk/detail/async_instance.hpp>  // IWYU pragma: export

#endif  // YADDNSC_SDK_ASYNC_DRIVER_HPP
