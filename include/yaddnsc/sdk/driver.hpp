//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_SDK_DRIVER_HPP
#define YADDNSC_SDK_DRIVER_HPP

/// C++ helper layer over the v1 alpha plugin C ABI (driver_abi.h).
///
/// Compiled into each driver plugin. Provides:
///   - owned/view wrappers for the C structs (HttpRequest, HttpResponse,
///     UpdateRequest, UpdateContext),
///   - the Driver base class with parse_config<T>() JSON deserialisation,
///   - YADDNSC_SDK_LOG_* logging macros capturing source location,
///   - YADDNSC_DEFINE_DRIVER, which emits the four C entry points.
///
/// Plugins must not include host-internal headers; this layer plus
/// driver_abi.h is the entire supported surface.

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glaze/glaze.hpp>

#include "driver_abi.h"
#include "format.hpp"
#include "redact.hpp"

#if defined(_WIN32)
#define YADDNSC_SDK_EXPORT __declspec(dllexport)
#else
#define YADDNSC_SDK_EXPORT __attribute__((visibility("default")))
#endif

namespace yaddnsc::sdk {

/* ── Errors ───────────────────────────────────────────────────────────────*/

/// Thrown by parse_config<T>() on malformed driver configuration JSON; the
/// generated entry points map it to YADDNSC_STATUS_INVALID_CONFIG.
class ConfigParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// An update failure reported by a Driver. `status` must be one of the
/// YADDNSC_STATUS_* constants other than YADDNSC_STATUS_OK.
struct Error {
    yaddnsc_status status = YADDNSC_STATUS_INTERNAL_ERROR;
    std::string message;
    uint32_t retry_after_seconds = 0;
};

using Result = std::expected<void, Error>;

/* ── HTTP ─────────────────────────────────────────────────────────────────*/

enum class Method : yaddnsc_http_method {
    Get = YADDNSC_HTTP_GET,
    Post = YADDNSC_HTTP_POST,
    Put = YADDNSC_HTTP_PUT,
    Delete = YADDNSC_HTTP_DELETE,
    Patch = YADDNSC_HTTP_PATCH,
    Head = YADDNSC_HTTP_HEAD,
    Options = YADDNSC_HTTP_OPTIONS,
};

struct HttpHeader {
    std::string name;
    std::string value;
};

/// An owned HTTP request handed to Services::exchange(). `body` engaged means
/// "send a body" (possibly empty); std::nullopt means no body at all.
struct HttpRequest {
    Method method = Method::Get;
    std::string url;
    std::vector<HttpHeader> headers;
    std::optional<std::string> body;
    std::string content_type;
};

struct HttpHeaderView {
    std::string_view name;
    std::string_view value;
};

/// A borrowed HTTP response. All views point into the host arena and stay
/// valid until the current yaddnsc_driver_update() call returns, across
/// multiple exchanges.
struct HttpResponse {
    uint32_t status_code = 0;
    std::vector<HttpHeaderView> headers;
    std::string_view body;
};

/// Transport/cancellation failure of an exchange. Provider HTTP status codes
/// (including 4xx/5xx) are NOT errors — they arrive in HttpResponse.
struct HttpError {
    yaddnsc_status status = YADDNSC_STATUS_NETWORK_ERROR;
    std::string message;
    uint32_t retry_after_seconds = 0;
};

using ExchangeResult = std::expected<HttpResponse, HttpError>;

/// Canonical verb text for a Method (same strings the host's wire formatter
/// emits; used by request logging).
[[nodiscard]] inline std::string_view method_name(Method method) noexcept {
    switch (method) {
        case Method::Get:
            return "GET";
        case Method::Post:
            return "POST";
        case Method::Put:
            return "PUT";
        case Method::Delete:
            return "DELETE";
        case Method::Patch:
            return "PATCH";
        case Method::Head:
            return "HEAD";
        case Method::Options:
            return "OPTIONS";
    }
    return "GET";
}

/// Render an HttpRequest for log output:
/// Request(body="...", content_type="...", method="...", header="...")
/// Sensitive header values and body/query parameters are redacted.
[[nodiscard]] inline std::string format_request(const HttpRequest &request) {
    std::string headers;
    for (const auto &[name, value]: request.headers) {
        headers.append(name);
        headers.append("=");
        headers.append(redact::redact_header(name, value));
        headers.append("; ");
    }
    if (headers.size() >= 2) {
        headers.erase(headers.size() - 2);
    }

    return fmt::format(R"(Request(body="{}", content_type="{}", method="{}", header="{}"))",
                       redact::redact_body(request.body.value_or("")), request.content_type,
                       method_name(request.method), headers);
}

/* ── Low-level view helpers ───────────────────────────────────────────────*/

namespace detail {

[[nodiscard]] inline yaddnsc_string make_view(std::string_view view) noexcept {
    return {view.data(), view.size()};
}

[[nodiscard]] inline yaddnsc_bytes make_bytes(std::string_view view) noexcept {
    return {reinterpret_cast<const uint8_t *>(view.data()), view.size()};
}

[[nodiscard]] inline std::string_view to_view(yaddnsc_string value) noexcept {
    return {value.data, value.size};
}

[[nodiscard]] inline std::string_view to_view(yaddnsc_bytes value) noexcept {
    return {reinterpret_cast<const char *>(value.data), value.size};
}

/// Write an error report honouring the caller-supplied capacity: fields are
/// only written when fully covered, struct_size is written back as
/// min(capacity, sizeof), and an unknown tail is never zeroed.
inline void write_error(yaddnsc_error *out_error, yaddnsc_status status, std::string_view message,
                        uint32_t retry_after_seconds) noexcept {
    if (out_error == nullptr || out_error->struct_size < YADDNSC_ERROR_MIN_SIZE) {
        return;
    }
    out_error->status = status;
    out_error->retry_after_seconds = retry_after_seconds;
    out_error->message = make_view(message);
    out_error->struct_size = out_error->struct_size < static_cast<uint32_t>(sizeof(yaddnsc_error))
                                 ? out_error->struct_size
                                 : static_cast<uint32_t>(sizeof(yaddnsc_error));
}

} // namespace detail

/* ── Services / UpdateContext ─────────────────────────────────────────────*/

/// Thin wrapper over the host services table. Copyable handle; the underlying
/// table is host-owned and valid from create() until the matching destroy().
class Services {
public:
    explicit Services(const yaddnsc_host_services *services) noexcept : services_(services) {}

    [[nodiscard]] const yaddnsc_host_services *get() const noexcept { return services_; }

    [[nodiscard]] bool is_cancelled() const noexcept {
        return services_ != nullptr && services_->is_cancelled != nullptr &&
               services_->is_cancelled(services_->context) != 0;
    }

    /// Perform an HTTP exchange through the host. Transport failures come
    /// back as HttpError; provider status codes arrive in HttpResponse.
    [[nodiscard]] ExchangeResult exchange(const HttpRequest &request) const {
        if (services_ == nullptr || services_->http_exchange == nullptr) {
            return std::unexpected(HttpError{YADDNSC_STATUS_NETWORK_ERROR, "host services unavailable", 0});
        }

        std::vector<yaddnsc_http_header> c_headers;
        c_headers.reserve(request.headers.size());
        for (const auto &[name, value]: request.headers) {
            c_headers.push_back({detail::make_view(name), detail::make_view(value)});
        }

        yaddnsc_http_request c_request{};
        c_request.struct_size = static_cast<uint32_t>(sizeof(c_request));
        c_request.url = detail::make_view(request.url);
        c_request.method = static_cast<yaddnsc_http_method>(request.method);
        c_request.headers = c_headers.data();
        c_request.header_count = c_headers.size();
        c_request.content_type = detail::make_view(request.content_type);
        if (request.body.has_value()) {
            c_request.body = detail::make_bytes(*request.body);
        }

        yaddnsc_http_response c_response{};
        c_response.struct_size = static_cast<uint32_t>(sizeof(c_response));
        yaddnsc_error c_error{};
        c_error.struct_size = static_cast<uint32_t>(sizeof(c_error));

        const yaddnsc_status status =
                services_->http_exchange(services_->context, &c_request, &c_response, &c_error);
        if (status != YADDNSC_STATUS_OK) {
            return std::unexpected(
                    HttpError{status, std::string(detail::to_view(c_error.message)), c_error.retry_after_seconds});
        }

        HttpResponse response;
        response.status_code = c_response.status_code;
        response.body = detail::to_view(c_response.body);
        response.headers.reserve(c_response.header_count);
        for (size_t i = 0; i < c_response.header_count; ++i) {
            response.headers.push_back({detail::to_view(c_response.headers[i].name),
                                        detail::to_view(c_response.headers[i].value)});
        }
        return response;
    }

private:
    const yaddnsc_host_services *services_;
};

/// The update parameters. All views are host-owned and valid for the
/// duration of Driver::update() only.
struct UpdateRequest {
    std::string_view ip_address;
    std::string_view record_type;
    std::string_view domain;
    std::string_view subdomain;
    std::string_view fqdn;
    std::string_view driver_param_json;
};

/// Everything a Driver needs for one update: the request parameters plus
/// access to the host services (logging, HTTP, cancellation).
class UpdateContext {
public:
    UpdateContext(const UpdateRequest &request, const yaddnsc_host_services *services) noexcept
        : request_(request), services_(services) {}

    [[nodiscard]] const UpdateRequest &request() const noexcept { return request_; }

    [[nodiscard]] Services services() const noexcept { return Services{services_}; }

    [[nodiscard]] ExchangeResult exchange(const HttpRequest &request) const { return services().exchange(request); }

    [[nodiscard]] bool is_cancelled() const noexcept { return services().is_cancelled(); }

private:
    UpdateRequest request_;
    const yaddnsc_host_services *services_;
};

namespace detail {

[[nodiscard]] inline const yaddnsc_host_services *to_services(const yaddnsc_host_services *services) noexcept {
    return services;
}

[[nodiscard]] inline const yaddnsc_host_services *to_services(const Services &services) noexcept {
    return services.get();
}

[[nodiscard]] inline const yaddnsc_host_services *to_services(const UpdateContext &context) noexcept {
    return context.services().get();
}

template<typename... Args>
inline void log_message(const yaddnsc_host_services *services, yaddnsc_log_level level, std::string_view file,
                        int32_t line, std::string_view function, std::format_string<Args...> fmt, Args &&...args) {
    if (services == nullptr || services->log == nullptr) {
        return;
    }
    const std::string message = fmt::format(fmt, std::forward<Args>(args)...);
    const yaddnsc_source_location location{make_view(file), line, make_view(function)};
    services->log(services->context, level, make_view(message), &location);
}

} // namespace detail

/* ── Driver base class ────────────────────────────────────────────────────*/

class Driver {
public:
    virtual ~Driver() = default;

    /// Perform one DNS record update. Return {} on success or an Error
    /// describing the failure. Implementations must be prepared for
    /// concurrent update() calls on distinct instances.
    virtual Result update(UpdateContext &context) = 0;

protected:
    /// Parse the driver_param JSON into a typed struct with built-in
    /// validation. Requires a glz::meta specialisation for T.
    /// @throws ConfigParseError when required keys are missing or values are
    ///         malformed.
    template<typename T>
    [[nodiscard]] static T parse_config(std::string_view driver_param_json) {
        T value{};
        const auto ec = glz::read<glz::opts{.error_on_missing_keys = true}>(value, driver_param_json, glz::context{});
        if (ec == glz::error_code::none) [[likely]] {
            return value;
        }

        throw ConfigParseError(fmt::format("Driver configuration parse error: {}", glz::format_error(ec)));
    }
};

/* ── Entry-point machinery (used by YADDNSC_DEFINE_DRIVER) ────────────────*/

namespace detail {

struct DriverInstance {
    std::unique_ptr<Driver> driver;
    yaddnsc_host_services services{};
    std::string error_storage;
};

template<typename DriverClass>
inline yaddnsc_status create_driver(const yaddnsc_host_services *services, yaddnsc_driver **out_driver,
                                    yaddnsc_error *out_error) {
    if (services == nullptr || out_driver == nullptr) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "services and out_driver must not be null", 0);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    if (services->struct_size < YADDNSC_HOST_SERVICES_MIN_SIZE ||
        services->api_revision != YADDNSC_DRIVER_API_REVISION || services->log == nullptr ||
        services->http_exchange == nullptr || services->is_cancelled == nullptr) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "incompatible host services table", 0);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }

    thread_local std::string create_error;
    *out_driver = nullptr;
    try {
        auto instance = std::make_unique<DriverInstance>();
        instance->services = *services;
        instance->driver = std::make_unique<DriverClass>();
        *out_driver = reinterpret_cast<yaddnsc_driver *>(instance.release()); // NOLINT
        return YADDNSC_STATUS_OK;
    } catch (const std::exception &e) {
        create_error = e.what();
    } catch (...) {
        create_error = "unknown exception during driver construction";
    }
    write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, create_error, 0);
    return YADDNSC_STATUS_INTERNAL_ERROR;
}

inline void destroy_driver(yaddnsc_driver *driver) noexcept {
    delete reinterpret_cast<DriverInstance *>(driver); // NOLINT — nullptr is a no-op
}

inline yaddnsc_status update_driver(yaddnsc_driver *driver, const yaddnsc_update_request *request,
                                    yaddnsc_error *out_error) {
    if (driver == nullptr || request == nullptr) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "driver and request must not be null", 0);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    if (request->struct_size < YADDNSC_UPDATE_REQUEST_MIN_SIZE) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "update request struct_size too small", 0);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }

    auto *instance = reinterpret_cast<DriverInstance *>(driver); // NOLINT
    try {
        const UpdateRequest update_request{
                .ip_address = to_view(request->ip_address),
                .record_type = to_view(request->record_type),
                .domain = to_view(request->domain),
                .subdomain = to_view(request->subdomain),
                .fqdn = to_view(request->fqdn),
                .driver_param_json = to_view(request->driver_param_json),
        };
        UpdateContext context{update_request, &instance->services};
        Result result = instance->driver->update(context);
        if (result.has_value()) {
            return YADDNSC_STATUS_OK;
        }

        Error &error = result.error();
        const yaddnsc_status status =
                error.status == YADDNSC_STATUS_OK ? YADDNSC_STATUS_INTERNAL_ERROR : error.status;
        instance->error_storage = std::move(error.message);
        write_error(out_error, status, instance->error_storage, error.retry_after_seconds);
        return status;
    } catch (const ConfigParseError &e) {
        instance->error_storage = e.what();
        write_error(out_error, YADDNSC_STATUS_INVALID_CONFIG, instance->error_storage, 0);
        return YADDNSC_STATUS_INVALID_CONFIG;
    } catch (const std::exception &e) {
        instance->error_storage = e.what();
    } catch (...) {
        instance->error_storage = "unknown exception during update";
    }
    write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, instance->error_storage, 0);
    return YADDNSC_STATUS_INTERNAL_ERROR;
}

} // namespace detail

} // namespace yaddnsc::sdk

/* ── Logging macros ───────────────────────────────────────────────────────*/

#define YADDNSC_SDK_LOG_TRACE(svc, ...) \
    ::yaddnsc::sdk::detail::log_message(::yaddnsc::sdk::detail::to_services(svc), YADDNSC_LOG_TRACE, __FILE__, \
                                        __LINE__, __FUNCTION__, __VA_ARGS__)
#define YADDNSC_SDK_LOG_DEBUG(svc, ...) \
    ::yaddnsc::sdk::detail::log_message(::yaddnsc::sdk::detail::to_services(svc), YADDNSC_LOG_DEBUG, __FILE__, \
                                        __LINE__, __FUNCTION__, __VA_ARGS__)
#define YADDNSC_SDK_LOG_INFO(svc, ...) \
    ::yaddnsc::sdk::detail::log_message(::yaddnsc::sdk::detail::to_services(svc), YADDNSC_LOG_INFO, __FILE__, \
                                        __LINE__, __FUNCTION__, __VA_ARGS__)
#define YADDNSC_SDK_LOG_WARN(svc, ...) \
    ::yaddnsc::sdk::detail::log_message(::yaddnsc::sdk::detail::to_services(svc), YADDNSC_LOG_WARN, __FILE__, \
                                        __LINE__, __FUNCTION__, __VA_ARGS__)
#define YADDNSC_SDK_LOG_ERROR(svc, ...) \
    ::yaddnsc::sdk::detail::log_message(::yaddnsc::sdk::detail::to_services(svc), YADDNSC_LOG_ERROR, __FILE__, \
                                        __LINE__, __FUNCTION__, __VA_ARGS__)

/* ── Driver definition macro ──────────────────────────────────────────────*/

/// Emit the four C entry points plus the static descriptor for a driver.
///
/// Usage in a driver plugin:
/// @code{.cpp}
///   class MyDriver final : public yaddnsc::sdk::Driver { ... };
///   YADDNSC_DEFINE_DRIVER(MyDriver, "my_driver", "description", "Author",
///                         "1.0.0", YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA)
/// @endcode
#define YADDNSC_DEFINE_DRIVER(DriverClass, driver_name, driver_description, driver_author, driver_version, \
                              driver_capabilities)                                                         \
    namespace {                                                                                            \
    constexpr yaddnsc_driver_descriptor YADDNSC_SDK_DESCRIPTOR = {                                         \
            .struct_size = sizeof(yaddnsc_driver_descriptor),                                              \
            .api_revision = YADDNSC_DRIVER_API_REVISION,                                                   \
            .magic = YADDNSC_DRIVER_MAGIC,                                                                 \
            .name = {driver_name, sizeof(driver_name) - 1},                                                \
            .version = {driver_version, sizeof(driver_version) - 1},                                       \
            .author = {driver_author, sizeof(driver_author) - 1},                                          \
            .description = {driver_description, sizeof(driver_description) - 1},                           \
            .capabilities = (driver_capabilities),                                                         \
    };                                                                                                     \
    }                                                                                                      \
                                                                                                           \
    extern "C" YADDNSC_SDK_EXPORT yaddnsc_status yaddnsc_driver_get_descriptor(                            \
            const yaddnsc_driver_descriptor **out_descriptor) {                                            \
        if (out_descriptor == nullptr) {                                                                   \
            return YADDNSC_STATUS_INVALID_ARGUMENT;                                                        \
        }                                                                                                  \
        *out_descriptor = &YADDNSC_SDK_DESCRIPTOR;                                                         \
        return YADDNSC_STATUS_OK;                                                                          \
    }                                                                                                      \
                                                                                                           \
    extern "C" YADDNSC_SDK_EXPORT yaddnsc_status yaddnsc_driver_create(                                    \
            const yaddnsc_host_services *services, yaddnsc_driver **out_driver, yaddnsc_error *out_error) { \
        return ::yaddnsc::sdk::detail::create_driver<DriverClass>(services, out_driver, out_error);        \
    }                                                                                                      \
                                                                                                           \
    extern "C" YADDNSC_SDK_EXPORT void yaddnsc_driver_destroy(yaddnsc_driver *driver) {                    \
        ::yaddnsc::sdk::detail::destroy_driver(driver);                                                    \
    }                                                                                                      \
                                                                                                           \
    extern "C" YADDNSC_SDK_EXPORT yaddnsc_status yaddnsc_driver_update(                                    \
            yaddnsc_driver *driver, const yaddnsc_update_request *request, yaddnsc_error *out_error) {     \
        return ::yaddnsc::sdk::detail::update_driver(driver, request, out_error);                          \
    }

#endif // YADDNSC_SDK_DRIVER_HPP
