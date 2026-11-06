#ifndef YADDNSC_SDK_ASYNC_DRIVER_HPP
#define YADDNSC_SDK_ASYNC_DRIVER_HPP

/// Async plugin SDK (ABI 1.1), source-level only.
///
/// This is a separate interface from the sync Driver/Services/UpdateContext.
/// It provides a lazy Task, bounded HTTP exchange groups, and an entry macro
/// that emits only the async execution group. It never exposes host coroutine
/// types, STL objects, or C++ objects across the C ABI. Child continuations
/// rely on the driver_abi.h contract that HTTP events follow, never interrupt,
/// the begin/resume step that accepted their commands.

#include <array>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <yaddnsc/sdk/driver_common.hpp>
#include <yaddnsc/sdk/log.hpp>
#include <yaddnsc/sdk/run_update.hpp>

namespace yaddnsc::sdk {

/// Maximum number of reserved plus active HTTP tasks in one async instance.
inline constexpr std::size_t MAX_ASYNC_EXCHANGES_PER_INSTANCE = 8;

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

/// A lazy, move-only task with structured, single-consumer continuation.
/// It does not expose the host loop, scheduler, or cancellation runtime.
template<typename T>
class Task {
public:
    struct promise_type;
    using Handle = std::coroutine_handle<promise_type>;

    struct promise_type {
        std::optional<T> value;
        std::exception_ptr error;
        std::coroutine_handle<> continuation{};
        bool started = false;

        struct FinalSuspend {
            [[nodiscard]] bool await_ready() const noexcept { return false; }

            [[nodiscard]] std::coroutine_handle<> await_suspend(Handle handle) const noexcept {
                const auto next = handle.promise().continuation;
                return next ? next : std::noop_coroutine();
            }

            void await_resume() const noexcept {}
        };

        [[nodiscard]] Task get_return_object() noexcept { return Task{Handle::from_promise(*this)}; }

        [[nodiscard]] std::suspend_always initial_suspend() noexcept { return {}; }

        [[nodiscard]] FinalSuspend final_suspend() noexcept { return {}; }

        void return_value(T result) { value.emplace(std::move(result)); }

        void unhandled_exception() noexcept { error = std::current_exception(); }
    };

    class Awaiter {
    public:
        explicit Awaiter(Handle handle) noexcept : handle_(handle) {}

        Awaiter(const Awaiter&) = delete;
        Awaiter& operator=(const Awaiter&) = delete;

        ~Awaiter() {
            if (handle_) {
                handle_.destroy();
            }
        }

        [[nodiscard]] bool await_ready() const noexcept { return handle_ && handle_.done(); }

        [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> parent) {
            if (!handle_) {
                throw std::logic_error("await requires a valid async task");
            }
            auto& promise = handle_.promise();
            if (promise.continuation && promise.continuation != parent) {
                throw std::logic_error("async task already has a continuation");
            }
            promise.continuation = parent;
            if (promise.started) {
                return std::noop_coroutine();
            }
            promise.started = true;
            return handle_;
        }

        T await_resume() {
            if (!handle_) {
                throw std::logic_error("await requires a valid async task");
            }
            auto& promise = handle_.promise();
            if (promise.error) {
                std::rethrow_exception(promise.error);
            }
            if (!promise.value.has_value()) {
                throw std::logic_error("async task completed without a result");
            }
            return std::move(*promise.value);
        }

    private:
        Handle handle_{};
    };

    Task() noexcept = default;

    explicit Task(Handle handle) noexcept : handle_(handle) {}

    /// A lazy task that completes with @p value when first resumed.
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

    [[nodiscard]] Awaiter operator co_await() && noexcept { return Awaiter{std::exchange(handle_, {})}; }

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(handle_); }

    [[nodiscard]] bool done() const noexcept { return handle_ && handle_.done(); }

    [[nodiscard]] Handle handle() const noexcept { return handle_; }

    void resume() {
        if (handle_ && !handle_.done()) {
            handle_.promise().started = true;
            handle_.resume();
        }
    }

    [[nodiscard]] T take_result() {
        if (!handle_) {
            throw std::logic_error("async task is not valid");
        }
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
class AsyncServices;
class AsyncUpdateContext;

namespace detail {

/// Internal hand-off from an awaitable to its owning instance. Host-side only:
/// the class and the awaitable mutators are defined in
/// <yaddnsc/sdk/detail/async_instance.hpp>, which this header includes last.
class AsyncExchangeHost;

/// Move-only creation-time reservation. Destruction releases an unstarted
/// reservation; completion releases an active one before business code resumes.
class ExchangeSlotLease {
public:
    ExchangeSlotLease() noexcept = default;

    ExchangeSlotLease(AsyncExchangeHost& host, std::size_t index) noexcept : host_(&host), index_(index) {}

    ~ExchangeSlotLease() noexcept;

    ExchangeSlotLease(ExchangeSlotLease&& other) noexcept;
    ExchangeSlotLease& operator=(ExchangeSlotLease&& other) noexcept;

    ExchangeSlotLease(const ExchangeSlotLease&) = delete;
    ExchangeSlotLease& operator=(const ExchangeSlotLease&) = delete;

    [[nodiscard]] std::optional<std::size_t> index() const noexcept { return index_; }

    void release() noexcept;

private:
    AsyncExchangeHost* host_ = nullptr;
    std::optional<std::size_t> index_;
};

struct ExchangeSlotBatch {
    std::array<ExchangeSlotLease, MAX_ASYNC_EXCHANGES_PER_INSTANCE> items{};
};

}  // namespace detail

/// Ordered group results plus update cancellation observed before completion.
/// A false flag is not a guarantee against a later cancellation; the host
/// remains the final authority at the update boundary.
struct AsyncWhenAllResult {
    std::vector<AsyncExchangeResult> results;
    bool update_cancelled_observed = false;
};

using AsyncWhenAllOperation = std::expected<Task<AsyncWhenAllResult>, HttpError>;

/// Selects which result wins an HTTP when_any group.
enum class AsyncWhenAnyPolicy {
    FIRST_COMPLETED,  // First response or HttpError.
    FIRST_SUCCESS,    // First result with has_value(), ignoring earlier HttpErrors.
};

/// Winner selection plus every result in original request order. Under
/// FIRST_SUCCESS, an absent winner means every request produced an HttpError.
/// Observed update-level cancellation clears the winner. A per-op CANCELLED
/// error can still win FIRST_COMPLETED while update_cancelled_observed is false.
struct AsyncWhenAnyResult {
    std::optional<std::size_t> winner_index;
    std::vector<AsyncExchangeResult> results;
    bool update_cancelled_observed = false;
};

using AsyncWhenAnyOperation = std::expected<Task<AsyncWhenAnyResult>, HttpError>;

/// Async host services and HTTP group factories. Group creation reserves all
/// required slots atomically; capacity failure creates no tasks or HTTP ops.
/// These operations are serial within one instance and are not thread-safe.
class AsyncServices {
public:
    AsyncServices(const yaddnsc_async_services* services, detail::AsyncExchangeHost* host) noexcept
        : services_(services), host_(host) {}

    [[nodiscard]] const yaddnsc_async_services* get() const noexcept { return services_; }

    /// Query the host's current cancellation state for this update.
    [[nodiscard]] bool is_cancelled() const noexcept {
        return services_ != nullptr && services_->is_cancelled != nullptr &&
               services_->is_cancelled(services_->context) != 0;
    }

    [[nodiscard]] int32_t (*log_enabled_fn() const noexcept)(void*, yaddnsc_log_level) {
        return services_ != nullptr ? services_->log_enabled : nullptr;
    }

    /// Create one HTTP awaitable and reserve its slot immediately. Capacity
    /// exhaustion is returned as an HttpError by co_await without an HTTP op.
    [[nodiscard]] ExchangeAwaitable exchange(HttpRequest request) const;

    /// Create a group that waits for every request and returns ordered results
    /// plus any update cancellation observed before the group completes.
    /// Capacity failure returns an error without starting any request.
    [[nodiscard]] AsyncWhenAllOperation when_all(std::vector<HttpRequest> requests) const;

    /// Create a group that selects a winner, cancels losers, and drains them.
    /// FIRST_SUCCESS uses AsyncExchangeResult::has_value(), so HTTP 4xx/5xx
    /// remain responses. Empty input has no winner and no results.
    [[nodiscard]] AsyncWhenAnyOperation when_any(std::vector<HttpRequest> requests,
                                                 AsyncWhenAnyPolicy policy = AsyncWhenAnyPolicy::FIRST_COMPLETED) const;

private:
    const yaddnsc_async_services* services_;
    detail::AsyncExchangeHost* host_;
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

    [[nodiscard]] AsyncWhenAllOperation when_all(std::vector<HttpRequest> requests) const;

    [[nodiscard]] AsyncWhenAnyOperation when_any(std::vector<HttpRequest> requests,
                                                 AsyncWhenAnyPolicy policy = AsyncWhenAnyPolicy::FIRST_COMPLETED) const;

private:
    UpdateRequest request_;
    AsyncServices services_;
};

/// One HTTP exchange with creation-time slot ownership. It owns the request
/// before suspension and copies the borrowed ABI response before resuming
/// business code. await_resume() transfers the owned result. The awaitable is
/// intentionally immovable because an active instance slot points to it.
class ExchangeAwaitable {
public:
    ExchangeAwaitable(detail::AsyncExchangeHost& host, HttpRequest request);
    ExchangeAwaitable(detail::AsyncExchangeHost& host, HttpRequest request, detail::ExchangeSlotLease lease,
                      const void* group_id);

    ExchangeAwaitable(const ExchangeAwaitable&) = delete;
    ExchangeAwaitable& operator=(const ExchangeAwaitable&) = delete;

    [[nodiscard]] bool await_ready() const noexcept { return admission_failed_; }

    bool await_suspend(std::coroutine_handle<> handle);

    [[nodiscard]] AsyncExchangeResult await_resume() noexcept { return std::move(result_); }

    /* ── Host-only ABI wiring; not for driver authors ───────────────────────*/

    [[nodiscard]] const yaddnsc_http_request& c_request() const noexcept;

    [[nodiscard]] const void* group_id() const noexcept { return group_id_; }

    [[nodiscard]] std::optional<std::size_t> slot_index() const noexcept { return lease_.index(); }

    void release_slot() noexcept;

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

    detail::AsyncExchangeHost* host_;
    HttpRequest request_;
    detail::ExchangeSlotLease lease_;
    const void* group_id_ = nullptr;
    bool admission_failed_ = false;
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

namespace detail {

using ExchangeTasks = std::array<Task<AsyncExchangeResult>, MAX_ASYNC_EXCHANGES_PER_INSTANCE>;

class StartedTasksAwaiter {
public:
    explicit StartedTasksAwaiter(ExchangeTasks& tasks) noexcept : tasks_(&tasks) {}

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> parent) {
        for (auto& task : *tasks_) {
            if (!task.valid() || task.done()) {
                continue;
            }
            auto& promise = task.handle().promise();
            if (promise.continuation && promise.continuation != parent) {
                throw std::logic_error("exchange child already has a continuation");
            }
            promise.continuation = parent;
        }
    }

    void await_resume() const noexcept {}

private:
    ExchangeTasks* tasks_;
};

inline Task<AsyncExchangeResult> run_reserved_exchange(AsyncExchangeHost& host, HttpRequest request,
                                                       ExchangeSlotLease lease, const void* group_id) {
    auto operation = ExchangeAwaitable{host, std::move(request), std::move(lease), group_id};
    co_return co_await operation;
}

[[nodiscard]] bool host_update_cancelled(AsyncExchangeHost& host) noexcept;
void cancel_exchange_group(AsyncExchangeHost& host, const void* group_id);

inline Task<AsyncWhenAnyResult> run_exchange_group(AsyncExchangeHost& host, std::vector<HttpRequest> requests,
                                                   ExchangeSlotBatch leases, std::optional<AsyncWhenAnyPolicy> policy) {
    const std::size_t count = requests.size();
    AsyncWhenAnyResult outcome;
    outcome.results.resize(count);
    ExchangeTasks children;
    std::array<bool, MAX_ASYNC_EXCHANGES_PER_INSTANCE> collected{};
    std::size_t completed = 0;
    std::optional<std::size_t> winner;
    bool cancellation_sent = false;

    for (std::size_t i = 0; i < count; ++i) {
        children[i] = run_reserved_exchange(host, std::move(requests[i]), std::move(leases.items[i]), &children);
    }
    for (std::size_t i = 0; i < count; ++i) {
        children[i].resume();
    }

    while (true) {
        if (host_update_cancelled(host)) {
            outcome.update_cancelled_observed = true;
            winner.reset();
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (collected[i] || !children[i].done()) {
                continue;
            }
            outcome.results[i] = children[i].take_result();
            collected[i] = true;
            ++completed;
            if (!outcome.update_cancelled_observed && policy.has_value() && !winner.has_value() &&
                (*policy == AsyncWhenAnyPolicy::FIRST_COMPLETED || outcome.results[i].has_value())) {
                winner = i;
            }
        }
        if (!outcome.update_cancelled_observed && winner.has_value() && !cancellation_sent) {
            cancellation_sent = true;
            cancel_exchange_group(host, &children);
        }
        if (completed == count) {
            outcome.winner_index = outcome.update_cancelled_observed ? std::nullopt : winner;
            co_return outcome;
        }
        co_await StartedTasksAwaiter{children};
    }
}

inline Task<AsyncWhenAllResult> run_when_all(AsyncExchangeHost& host, std::vector<HttpRequest> requests,
                                             ExchangeSlotBatch leases) {
    auto outcome = co_await run_exchange_group(host, std::move(requests), std::move(leases), std::nullopt);
    co_return AsyncWhenAllResult{.results = std::move(outcome.results),
                                 .update_cancelled_observed = outcome.update_cancelled_observed};
}

}  // namespace detail

/* ── Driver base ──────────────────────────────────────────────────────────*/

class AsyncDriver : public DriverHelpers {
public:
    virtual ~AsyncDriver() noexcept = default;

    /// Start one update. The returned task is resumed by the async instance;
    /// it may await single exchanges or bounded when_all/when_any groups.
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
        detail::log_update_request(context.services(), context.request(), driver_name, request, location);
        auto operation = context.exchange(request);
        auto response = co_await operation;
        co_return detail::finish_update(context.services(), context.request(), response, check_response, location);
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

/// Plugin-side ABI entry machinery used by the definition macro. Included last so
/// that including <yaddnsc/sdk/async_driver.hpp> alone still resolves it.
#include <yaddnsc/sdk/detail/async_instance.hpp>  // IWYU pragma: export

#endif  // YADDNSC_SDK_ASYNC_DRIVER_HPP
