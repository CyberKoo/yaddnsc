#ifndef YADDNSC_SDK_DRIVER_HPP
#define YADDNSC_SDK_DRIVER_HPP

/// C++ helper layer over the v1 plugin C ABI (driver_abi.h).
///
/// This header is a source-level helper compiled into each plugin. It is not
/// itself an ABI: exceptions, STL containers, and C++ objects stay on this
/// side of the boundary. The generated entry points catch every exception and
/// report it through the C ABI without allocating on that path.
///
/// Compiled into each driver plugin. Provides:
///   - owned/view wrappers for the C structs (HttpRequest, HttpResponse,
///     UpdateRequest, UpdateContext),
///   - the Driver base class with parse_config<T>() / parse_response<T>()
///     JSON deserialisation helpers,
///   - YADDNSC_SDK_LOG_* logging macros capturing source location,
///   - YADDNSC_DEFINE_DRIVER, which emits the four required C entry points
///     plus the optional validate entry,
///
/// The async SDK is a separate source surface in
/// <yaddnsc/sdk/async_driver.hpp>; the two service/context families do not
/// convert into each other.
///
/// Plugins must not include host-internal headers; this layer plus
/// the public SDK/util headers form the supported source surface.

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <concepts>

#include <expected>
#include <glaze/glaze.hpp>
#include <yaddnsc/util/string_util.hpp>

#include "yaddnsc/sdk/driver_abi.h"
#include "yaddnsc/sdk/format.hpp"  // IWYU pragma: keep — clangd misses fmt:: usage in uninstantiated template bodies
#include "yaddnsc/sdk/redact.hpp"

#define YADDNSC_SDK_EXPORT __attribute__((visibility("default")))

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
    GET = YADDNSC_HTTP_GET,
    POST = YADDNSC_HTTP_POST,
    PUT = YADDNSC_HTTP_PUT,
    DELETE = YADDNSC_HTTP_DELETE,
    PATCH = YADDNSC_HTTP_PATCH,
    HEAD = YADDNSC_HTTP_HEAD,
    OPTIONS = YADDNSC_HTTP_OPTIONS,
};

struct HttpHeader {
    std::string name;
    std::string value;
};

/// An owned HTTP request handed to Services::exchange(). `body` engaged means
/// "send a body" (possibly empty); std::nullopt means no body at all.
struct HttpRequest {
    Method method = Method::GET;
    std::string url;
    std::vector<HttpHeader> headers;
    std::optional<std::string> body;
    std::string content_type;
};

struct HttpHeaderView {
    std::string_view name;
    std::string_view value;
};

/// A borrowed synchronous HTTP response. Every view points into the host
/// arena and stays valid until the current yaddnsc_driver_update() call
/// returns, across multiple exchanges. Copying the response does not extend
/// that lifetime.
/// Header names keep the peer's casing and may repeat; compare them
/// case-insensitively.
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
        case Method::GET:
            return "GET";
        case Method::POST:
            return "POST";
        case Method::PUT:
            return "PUT";
        case Method::DELETE:
            return "DELETE";
        case Method::PATCH:
            return "PATCH";
        case Method::HEAD:
            return "HEAD";
        case Method::OPTIONS:
            return "OPTIONS";
    }
    return "GET";
}

/// Render an HttpRequest for log output:
/// Request(body="...", content_type="...", method="...", header="...")
/// Sensitive header values and body/query parameters are redacted; the body
/// is truncated to the log budget.
[[nodiscard]] inline std::string format_request(const HttpRequest& request) {
    std::string headers;
    for (const auto& [name, value] : request.headers) {
        headers.append(name);
        headers.append("=");
        headers.append(redact::redact_header(name, value));
        headers.append("; ");
    }
    if (headers.size() >= 2) {
        headers.erase(headers.size() - 2);
    }

    return fmt::format(R"(Request(body="{}", content_type="{}", method="{}", url="{}", header="{}"))",
                       redact::truncate_for_log(redact::redact_body(request.body.value_or(""), request.content_type)),
                       request.content_type, method_name(request.method), redact::redact_url_query(request.url),
                       headers);
}

/* ── Low-level view helpers ───────────────────────────────────────────────*/

namespace detail {

/// Descriptor strings must be constexpr NUL-terminated arrays, never pointers.
template<std::size_t N>
[[nodiscard]] consteval yaddnsc_string descriptor_string(const char (&value)[N]) {
    static_assert(N > 0);
    if (value[N - 1] != '\0') {
        throw "descriptor strings must be NUL-terminated";
    }
    return {value, N - 1};
}

/// Copy only fields covered by the provider's capacity; never read its tail.
[[nodiscard]] inline yaddnsc_host_services copy_services(const yaddnsc_host_services& source) noexcept {
    yaddnsc_host_services result{};
    std::memcpy(&result, &source, source.struct_size < sizeof(result) ? source.struct_size : sizeof(result));
    result.struct_size =
        static_cast<uint32_t>(source.struct_size < sizeof(result) ? source.struct_size : sizeof(result));
    return result;
}

/// First host-services minor that carries the appended log_enabled field.
inline constexpr uint16_t HOST_SERVICES_V1_1_MINOR = 1;

/// Copy the 1.0 prefix and, only when the provider advertises a complete ABI
/// 1.1 container, the optional log query. The source must point at the base
/// subobject of a yaddnsc_host_services_v1_1; the version and size checks are
/// the provider's contract, not something the SDK can verify for a raw table.
[[nodiscard]] inline yaddnsc_host_services_v1_1 copy_services_v1_1(const yaddnsc_host_services& source) noexcept {
    yaddnsc_host_services_v1_1 result{};
    result.base = copy_services(source);
    if (yaddnsc_struct_has_field(source.struct_size, YADDNSC_HOST_SERVICES_V1_1_MIN_SIZE) &&
        yaddnsc_abi_provides(source.abi_major, source.abi_minor, YADDNSC_DRIVER_ABI_MAJOR,
                             HOST_SERVICES_V1_1_MINOR)) {
        // A complete 1.1 provider passed &container.base. Copy only the known
        // field; a larger provider tail stays unread.
        const auto* extended = reinterpret_cast<const yaddnsc_host_services_v1_1*>(&source);  // NOLINT
        result.log_enabled = extended->log_enabled;
        result.base.struct_size = static_cast<uint32_t>(sizeof(yaddnsc_host_services_v1_1));
    }
    return result;
}

[[nodiscard]] inline yaddnsc_string make_view(std::string_view view) noexcept {
    return {view.data(), view.size()};
}

[[nodiscard]] inline yaddnsc_bytes make_bytes(std::string_view view) noexcept {
    return {reinterpret_cast<const uint8_t*>(view.data()), view.size()};
}

[[nodiscard]] inline std::string_view to_view(yaddnsc_string value) noexcept {
    return value.data == nullptr ? std::string_view{} : std::string_view{value.data, value.size};
}

[[nodiscard]] inline std::string_view to_view(yaddnsc_bytes value) noexcept {
    return value.data == nullptr ? std::string_view{}
                                 : std::string_view{reinterpret_cast<const char*>(value.data), value.size};
}

/// Copy @p message into a fixed thread-local buffer without allocating.
/// A @p message with null data selects @p fallback. The returned view is
/// valid until the next call on this thread and must be copied by the caller
/// before then.
[[nodiscard]] inline std::string_view copy_bounded(std::string_view message, std::string_view fallback) noexcept {
    constexpr std::size_t capacity = 512;
    thread_local std::array<char, capacity> storage{};
    const std::string_view source = message.data() == nullptr ? fallback : message;
    const std::size_t size = source.size() < (storage.size() - 1) ? source.size() : (storage.size() - 1);
    if (size != 0) {
        std::memcpy(storage.data(), source.data(), size);
    }
    storage[size] = '\0';
    return {storage.data(), size};
}

/// Write an error report honouring the caller-supplied capacity: fields are
/// only written when fully covered, struct_size is written back as
/// min(capacity, sizeof), and an unknown tail is never zeroed. A null
/// @p out_error, or one whose struct_size is below the v1 layout, is left
/// unchanged.
inline void write_error(yaddnsc_error* out_error, yaddnsc_status status, std::string_view message,
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

[[nodiscard]] inline bool has_valid_error(const yaddnsc_error& error) noexcept {
    return error.struct_size >= YADDNSC_ERROR_MIN_SIZE && yaddnsc_status_is_valid(error.status) &&
           yaddnsc_string_is_valid(error.message);
}

[[nodiscard]] inline bool has_valid_response(const yaddnsc_http_response& response) noexcept {
    if (response.struct_size < YADDNSC_HTTP_RESPONSE_MIN_SIZE || !yaddnsc_bytes_is_valid(response.body) ||
        !yaddnsc_http_header_array_is_valid(response.headers, response.header_count)) {
        return false;
    }
    for (size_t i = 0; i < response.header_count; ++i) {
        if (!yaddnsc_http_header_is_valid(response.headers[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace detail

/* ── Services / UpdateContext ─────────────────────────────────────────────*/

/// Thin wrapper over the host services table. Copyable handle; the underlying
/// table is host-owned and valid from create() until the matching destroy().
class Services {
public:
    using LogEnabledFn = int32_t (*)(void* context, yaddnsc_log_level level);

    explicit Services(const yaddnsc_host_services* services) noexcept : services_(services) {}

    /// Internal SDK path: carries the optional query validated by the factory.
    /// The one-argument constructor never probes a raw table's tail.
    Services(const yaddnsc_host_services* services, LogEnabledFn log_enabled) noexcept
        : services_(services), log_enabled_(log_enabled) {}

    [[nodiscard]] const yaddnsc_host_services* get() const noexcept { return services_; }

    [[nodiscard]] LogEnabledFn log_enabled_fn() const noexcept { return log_enabled_; }

    [[nodiscard]] bool is_cancelled() const noexcept {
        return services_ != nullptr && services_->is_cancelled != nullptr &&
               services_->is_cancelled(services_->context) != 0;
    }

    /// Perform an HTTP exchange through the host. Transport failures come
    /// back as HttpError; provider status codes arrive in HttpResponse.
    [[nodiscard]] ExchangeResult exchange(const HttpRequest& request) const {
        if (services_ == nullptr || services_->struct_size < YADDNSC_ABI_VERSION_PREFIX_SIZE ||
            !yaddnsc_abi_provides(services_->abi_major, services_->abi_minor, YADDNSC_DRIVER_ABI_MAJOR, 0) ||
            services_->struct_size < YADDNSC_HOST_SERVICES_MIN_SIZE || services_->context == nullptr ||
            services_->http_exchange == nullptr) {
            return std::unexpected(HttpError{YADDNSC_STATUS_INTERNAL_ERROR, "invalid host services table", 0});
        }

        std::vector<yaddnsc_http_header> c_headers;
        c_headers.reserve(request.headers.size());
        for (const auto& [name, value] : request.headers) {
            c_headers.push_back({detail::make_view(name), detail::make_view(value)});
        }

        yaddnsc_http_request c_request{};
        c_request.struct_size = static_cast<uint32_t>(sizeof(c_request));
        c_request.url = detail::make_view(request.url);
        c_request.method = static_cast<yaddnsc_http_method>(request.method);
        c_request.headers = c_headers.empty() ? nullptr : c_headers.data();
        c_request.header_count = c_headers.size();
        c_request.content_type = detail::make_view(request.content_type);
        if (request.body.has_value()) {
            c_request.body = detail::make_bytes(*request.body);
        }

        yaddnsc_http_response c_response{};
        c_response.struct_size = static_cast<uint32_t>(sizeof(c_response));
        yaddnsc_error c_error{};
        c_error.struct_size = static_cast<uint32_t>(sizeof(c_error));

        const yaddnsc_status status = services_->http_exchange(services_->context, &c_request, &c_response, &c_error);
        if (!yaddnsc_status_is_valid(status)) {
            return std::unexpected(HttpError{YADDNSC_STATUS_INTERNAL_ERROR, "host returned an unknown status", 0});
        }
        if (status != YADDNSC_STATUS_OK) {
            if (!detail::has_valid_error(c_error) || c_error.status != status) {
                return std::unexpected(
                    HttpError{YADDNSC_STATUS_INTERNAL_ERROR, "host returned an invalid error report", 0});
            }
            return std::unexpected(
                HttpError{status, std::string(detail::to_view(c_error.message)), c_error.retry_after_seconds});
        }

        if (!detail::has_valid_response(c_response)) {
            return std::unexpected(
                HttpError{YADDNSC_STATUS_INTERNAL_ERROR, "host returned an invalid HTTP response", 0});
        }

        HttpResponse response;
        response.status_code = c_response.status_code;
        response.body = detail::to_view(c_response.body);
        response.headers.reserve(c_response.header_count);
        for (size_t i = 0; i < c_response.header_count; ++i) {
            response.headers.push_back(
                {detail::to_view(c_response.headers[i].name), detail::to_view(c_response.headers[i].value)});
        }
        return response;
    }

private:
    const yaddnsc_host_services* services_;
    LogEnabledFn log_enabled_ = nullptr;
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
    using LogEnabledFn = Services::LogEnabledFn;

    UpdateContext(const UpdateRequest& request, const yaddnsc_host_services* services) noexcept
        : UpdateContext(request, services, nullptr) {}

    /// Internal SDK path: carries the optional log query from the factory.
    UpdateContext(const UpdateRequest& request, const yaddnsc_host_services* services,
                  LogEnabledFn log_enabled) noexcept
        : request_(request), services_(services), log_enabled_(log_enabled) {}

    [[nodiscard]] const UpdateRequest& request() const noexcept { return request_; }

    [[nodiscard]] Services services() const noexcept { return Services{services_, log_enabled_}; }

    [[nodiscard]] ExchangeResult exchange(const HttpRequest& request) const { return services().exchange(request); }

    [[nodiscard]] bool is_cancelled() const noexcept { return services().is_cancelled(); }

private:
    UpdateRequest request_;
    const yaddnsc_host_services* services_;
    LogEnabledFn log_enabled_ = nullptr;
};

namespace detail {

template<typename... Args>
inline void log_message(const yaddnsc_host_services* services, yaddnsc_log_level level, std::string_view file,
                        int32_t line, std::string_view function, std::format_string<Args...> fmt,
                        Args&&... args) noexcept {
    if (services == nullptr || services->log == nullptr) {
        return;
    }
    // Logging must never fail an update. Format allocation and a throwing
    // host callback are dropped here; the C entry point does not see them.
    try {
        const std::string message = fmt::format(fmt, std::forward<Args>(args)...);
        const yaddnsc_source_location location{make_view(file), line, make_view(function)};
        services->log(services->context, level, make_view(message), &location);
    } catch (...) {
    }
}

struct LogTarget {
    const yaddnsc_host_services* services = nullptr;
    Services::LogEnabledFn log_enabled = nullptr;
};

[[nodiscard]] inline LogTarget to_log_target(const yaddnsc_host_services* services) noexcept {
    return {services, nullptr};
}

[[nodiscard]] inline LogTarget to_log_target(const Services& services) noexcept {
    return {services.get(), services.log_enabled_fn()};
}

[[nodiscard]] inline LogTarget to_log_target(const UpdateContext& context) noexcept {
    return to_log_target(context.services());
}

/// Best-effort diagnostic boundary including service lookup, level evaluation,
/// the optional level query, argument evaluation and formatting. The log
/// callable runs only when a log callback exists and the host did not disable
/// the level. All diagnostic failures are discarded; they never change the
/// business-operation result.
template<typename GetLevel, typename GetServices, typename Log>
inline void log_lazy(GetLevel&& get_level, GetServices&& get_services, Log&& log) noexcept {
    try {
        const LogTarget target = to_log_target(get_services());
        if (target.services == nullptr || target.services->log == nullptr) {
            return;
        }
        const yaddnsc_log_level level = get_level();
        if (target.log_enabled != nullptr) {
            bool enabled = true;
            try {
                enabled = target.log_enabled(target.services->context, level) != 0;
            } catch (...) {
                enabled = true;  // best effort: an unusable query keeps the log
            }
            if (!enabled) {
                return;
            }
        }
        log(target.services, level);
    } catch (...) {
    }
}

/// Howard Hinnant's public-domain civil-from-days companion, inverted:
/// days since 1970-01-01 for a Gregorian date.
[[nodiscard]] inline int days_from_civil(int year, unsigned month, unsigned day) noexcept {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + static_cast<unsigned>(month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int>(doe) - 719468;
}

[[nodiscard]] inline int month_index(std::string_view name) noexcept {
    constexpr std::array<std::string_view, 12> months = {"jan", "feb", "mar", "apr", "may", "jun",
                                                         "jul", "aug", "sep", "oct", "nov", "dec"};
    for (std::size_t i = 0; i < months.size(); ++i) {
        if (util::iequals(name, months[i])) {
            return static_cast<int>(i) + 1;
        }
    }
    return 0;
}

/// IMF-fixdate (`Sun, 06 Nov 1994 08:49:37 GMT`). The weekday is skipped.
[[nodiscard]] inline std::optional<std::chrono::sys_seconds> parse_http_date(std::string_view text) noexcept {
    const auto comma = text.find(',');
    if (comma == std::string_view::npos || comma + 1 >= text.size()) {
        return std::nullopt;
    }
    text.remove_prefix(comma + 1);
    text = util::trim(text);

    unsigned day = 0;
    const auto [day_end, day_ec] = std::from_chars(text.data(), text.data() + text.size(), day);
    if (day_ec != std::errc{} || day < 1 || day > 31) {
        return std::nullopt;
    }
    text.remove_prefix(static_cast<std::size_t>(day_end - text.data()));
    if (text.empty() || text.front() != ' ') {
        return std::nullopt;
    }
    text.remove_prefix(1);
    if (text.size() < 3) {
        return std::nullopt;
    }
    const int month = month_index(text.substr(0, 3));
    if (month == 0 || text.size() < 4 || text[3] != ' ') {
        return std::nullopt;
    }
    text.remove_prefix(4);

    unsigned year = 0;
    const auto [year_end, year_ec] = std::from_chars(text.data(), text.data() + text.size(), year);
    if (year_ec != std::errc{} || year < 1970 || year > 9999) {
        return std::nullopt;
    }
    text.remove_prefix(static_cast<std::size_t>(year_end - text.data()));
    if (text.size() < 10 || text.front() != ' ' || text[3] != ':' || text[6] != ':') {
        return std::nullopt;
    }
    text.remove_prefix(1);

    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;
    const auto [hour_end, hour_ec] = std::from_chars(text.data(), text.data() + 2, hour);
    const auto [minute_end, minute_ec] = std::from_chars(text.data() + 3, text.data() + 5, minute);
    const auto [second_end, second_ec] = std::from_chars(text.data() + 6, text.data() + 8, second);
    if (hour_ec != std::errc{} || minute_ec != std::errc{} || second_ec != std::errc{} || hour_end != text.data() + 2 ||
        minute_end != text.data() + 5 || second_end != text.data() + 8 || hour > 23 || minute > 59 || second > 60) {
        return std::nullopt;
    }
    text.remove_prefix(8);
    text = util::trim(text);
    if (!util::iequals(text, "GMT")) {
        return std::nullopt;
    }

    const auto days = days_from_civil(static_cast<int>(year), static_cast<unsigned>(month), day);
    const auto secs = static_cast<std::int64_t>(days) * 86400 + static_cast<std::int64_t>(hour) * 3600 +
                      static_cast<std::int64_t>(minute) * 60 + static_cast<std::int64_t>(second);
    return std::chrono::sys_seconds{std::chrono::seconds{secs}};
}

/// First Retry-After relative to @p now. Delay-seconds win; a past date or
/// an unparsable value is 0. The explicit clock sample keeps date arithmetic
/// independent of wall-clock changes between parsing and comparison.
template<typename Response>
[[nodiscard]] inline std::uint32_t retry_after_seconds(const Response& response,
                                                       std::chrono::sys_seconds now) noexcept {
    for (const auto& header : response.headers) {
        if (!util::iequals(header.name, "retry-after")) {
            continue;
        }
        const auto value = util::trim(header.value);
        std::uint32_t seconds = 0;
        const char* const begin = value.data();
        const char* const end = begin + value.size();
        const auto [ptr, ec] = std::from_chars(begin, end, seconds);
        if (ec == std::errc{} && ptr == end) {
            return seconds;
        }
        if (ec == std::errc::result_out_of_range && ptr == end) {
            return std::numeric_limits<std::uint32_t>::max();
        }
        const auto date = parse_http_date(value);
        if (!date.has_value()) {
            return 0;
        }
        if (*date <= now) {
            return 0;
        }
        const auto delta = (*date - now).count();
        if (delta >= static_cast<std::chrono::seconds::rep>(std::numeric_limits<std::uint32_t>::max())) {
            return std::numeric_limits<std::uint32_t>::max();
        }
        return static_cast<std::uint32_t>(delta);
    }
    return 0;
}

/// Production boundary: sample the clock once, then use the same pure
/// calculation exercised with fixed timestamps in the SDK tests.
template<typename Response>
[[nodiscard]] inline std::uint32_t retry_after_seconds(const Response& response) noexcept {
    return retry_after_seconds(response,
                               std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now()));
}

struct UpstreamFailure {
    yaddnsc_status status = YADDNSC_STATUS_UPSTREAM_REJECTED;
    std::uint32_t retry_after_seconds = 0;
};

/// Status for an exchange the driver has already rejected. 401/403 are
/// authentication failures. 429 carries Retry-After. Everything else stays
/// an upstream rejection with no retry hint.
template<typename Response>
[[nodiscard]] inline UpstreamFailure classify_upstream_failure(const Response& response) noexcept {
    if (response.status_code == 401 || response.status_code == 403) {
        return {YADDNSC_STATUS_AUTHENTICATION_FAILED, 0};
    }
    if (response.status_code == 429) {
        return {YADDNSC_STATUS_RATE_LIMITED, retry_after_seconds(response)};
    }
    return {};
}

/// Whether an upstream rejection clears without operator action: rate
/// limiting and server-side failures are retried by the next update cycle,
/// so a driver reports them as degradation rather than as a failed unit of
/// work. Every other status (auth, validation, not found) stays a failure.
[[nodiscard]] constexpr bool is_transient_status(std::uint32_t status_code) noexcept {
    return status_code == 429 || (status_code >= 500 && status_code < 600);
}

}  // namespace detail

/* ── Driver base class ────────────────────────────────────────────────────*/

/// Source-level helpers shared by the sync and async driver bases.
class DriverHelpers {
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

    /// Parse an upstream API response body into a typed struct. Providers
    /// return fields beyond those modeled in T and extend their responses
    /// over time, so unknown keys are ignored rather than failing the parse
    /// (glaze errors on them by default). Requires a glz::meta
    /// specialisation for T. Returns std::nullopt when the body does not
    /// match the expected shape.
    template<typename T>
    [[nodiscard]] static std::optional<T> parse_response(std::string_view body) {
        T value{};
        const auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(value, body, glz::context{});
        if (ec == glz::error_code::none) [[likely]] {
            return value;
        }

        return std::nullopt;
    }
};

class Driver : public DriverHelpers {
public:
    virtual ~Driver() noexcept = default;

    /// Perform one DNS record update. Return {} on success or an Error
    /// describing the failure. Implementations must be prepared for
    /// concurrent update() calls on distinct instances.
    virtual Result update(UpdateContext& context) = 0;

    /// Validate a driver_param JSON against this driver's schema without
    /// performing an update. The host calls it from `config test` through the
    /// OPTIONAL yaddnsc_driver_validate entry (dlsym-probed).
    /// YADDNSC_DEFINE_DRIVER always exports that entry. Each driver must
    /// explicitly validate its schema and semantics; parameterless drivers
    /// may return success. Reuse the same parser in validate and update.
    /// ConfigParseError maps to YADDNSC_STATUS_INVALID_CONFIG. Validation
    /// uses a fresh instance and must not perform HTTP exchanges.
    [[nodiscard]] virtual Result validate(std::string_view driver_param_json) const = 0;

protected:
    /// Run the canonical update tail shared by the bundled drivers: log the
    /// outgoing request, perform the exchange, and translate transport
    /// failures and upstream rejections into an Error. @p check_response
    /// receives the response plus a Services handle and decides whether the
    /// provider accepted the update. A rejection of HTTP 401 or 403 is
    /// AUTHENTICATION_FAILED. A rejection of HTTP 429 is RATE_LIMITED and
    /// carries Retry-After. Any other rejection is UPSTREAM_REJECTED.
    /// A capturing callable may instead return Result; its detailed Error is
    /// propagated unchanged. The bool checker form remains source-compatible.
    template<typename CheckResponse>
    static Result run_update(UpdateContext& context, std::string_view driver_name, const HttpRequest& request,
                             CheckResponse&& check_response,
                             const std::source_location& location = std::source_location::current()) {
        const auto& params = context.request();
        const auto file = location.file_name();
        const auto line = static_cast<int32_t>(location.line());
        const auto function = location.function_name();

        detail::log_lazy([&] { return YADDNSC_LOG_DEBUG; }, [&] { return context.services(); },
                         [&](const auto* services, yaddnsc_log_level level) {
                             detail::log_message(
                                 services, level, file, line, function,
                                 "Domain \"{}\" ({}) received DNS record update request from driver '{}', {}",
                                 params.fqdn, params.record_type, driver_name, format_request(request));
                         });

        auto response = context.exchange(request);
        if (!response) {
            detail::log_lazy([&] { return YADDNSC_LOG_WARN; }, [&] { return context.services(); },
                             [&](const auto* services, yaddnsc_log_level level) {
                                 detail::log_message(services, level, file, line, function,
                                                     "Domain \"{}\" ({}) update failed: {}", params.fqdn,
                                                     params.record_type, response.error().message);
                             });
            return std::unexpected(
                Error{response.error().status, response.error().message, response.error().retry_after_seconds});
        }

        auto checked = std::invoke(std::forward<CheckResponse>(check_response), *response, context.services());
        if (!checked) {
            if constexpr (std::same_as<decltype(checked), Result>) {
                return checked;  // Preserve the checker's detailed parse/provider error.
            }
            const auto failure = detail::classify_upstream_failure(*response);
            detail::log_lazy([&] { return YADDNSC_LOG_WARN; }, [&] { return context.services(); },
                             [&](const auto* services, yaddnsc_log_level level) {
                                 detail::log_message(services, level, file, line, function,
                                                     "Domain \"{}\" ({}) update rejected by upstream",
                                                     params.fqdn, params.record_type);
                             });
            return std::unexpected(
                Error{failure.status,
                      fmt::format("Domain {} ({}) update rejected by upstream", params.fqdn, params.record_type),
                      failure.retry_after_seconds});
        }

        return {};
    }
};

/* ── Entry-point machinery (used by YADDNSC_DEFINE_DRIVER) ────────────────*/

namespace detail {

struct DriverInstance {
    // Driver destruction may log through injected Services; destroy it first.
    yaddnsc_host_services_v1_1 services{};
    std::string error_storage;
    std::unique_ptr<Driver> driver;
};

template<typename DriverClass, uint16_t RequiredMinor = 0>
inline yaddnsc_status create_driver(const yaddnsc_host_services* services, yaddnsc_driver** out_driver,
                                    yaddnsc_error* out_error) noexcept {
    if (services == nullptr || out_driver == nullptr) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "services and out_driver must not be null", 0);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    *out_driver = nullptr;
    if (services->struct_size < YADDNSC_ABI_VERSION_PREFIX_SIZE ||
        !yaddnsc_abi_provides(services->abi_major, services->abi_minor, YADDNSC_DRIVER_ABI_MAJOR, RequiredMinor) ||
        services->struct_size < YADDNSC_HOST_SERVICES_MIN_SIZE || services->context == nullptr ||
        services->log == nullptr || services->http_exchange == nullptr || services->is_cancelled == nullptr) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "incompatible host services table", 0);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }

    static_assert(std::is_nothrow_destructible_v<DriverClass>,
                  "driver classes must be nothrow destructible across the C ABI");
    constexpr std::string_view fallback = "unknown exception during driver construction";
    std::string_view reported;
    try {
        auto instance = std::make_unique<DriverInstance>();
        instance->services = copy_services_v1_1(*services);
        if constexpr (std::is_constructible_v<DriverClass, Services>) {
            instance->driver = std::make_unique<DriverClass>(
                Services{&instance->services.base, instance->services.log_enabled});
        } else {
            instance->driver = std::make_unique<DriverClass>();
        }
        *out_driver = reinterpret_cast<yaddnsc_driver*>(instance.release());  // NOLINT
        return YADDNSC_STATUS_OK;
    } catch (const std::exception& e) {
        reported = copy_bounded(e.what(), fallback);
    } catch (...) {
        reported = copy_bounded({}, fallback);
    }
    write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, reported, 0);
    return YADDNSC_STATUS_INTERNAL_ERROR;
}

inline void destroy_driver(yaddnsc_driver* driver) noexcept {
    delete reinterpret_cast<DriverInstance*>(driver);  // NOLINT — nullptr is a no-op
}

inline yaddnsc_status update_driver(yaddnsc_driver* driver, const yaddnsc_update_request* request,
                                    yaddnsc_error* out_error) noexcept {
    if (driver == nullptr || request == nullptr) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "driver and request must not be null", 0);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    if (request->struct_size < YADDNSC_UPDATE_REQUEST_MIN_SIZE) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "update request struct_size too small", 0);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    if (!yaddnsc_string_is_valid(request->ip_address) || !yaddnsc_string_is_valid(request->record_type) ||
        !yaddnsc_string_is_valid(request->domain) || !yaddnsc_string_is_valid(request->subdomain) ||
        !yaddnsc_string_is_valid(request->fqdn) || !yaddnsc_bytes_is_valid(request->driver_param_json)) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "update request contains an invalid view", 0);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }

    auto* instance = reinterpret_cast<DriverInstance*>(driver);  // NOLINT
    try {
        const UpdateRequest update_request{
            .ip_address = to_view(request->ip_address),
            .record_type = to_view(request->record_type),
            .domain = to_view(request->domain),
            .subdomain = to_view(request->subdomain),
            .fqdn = to_view(request->fqdn),
            .driver_param_json = to_view(request->driver_param_json),
        };
        UpdateContext context{update_request, &instance->services.base, instance->services.log_enabled};
        Result result = instance->driver->update(context);
        if (result.has_value()) {
            return YADDNSC_STATUS_OK;
        }

        Error& error = result.error();
        const yaddnsc_status status = !yaddnsc_status_is_valid(error.status) || error.status == YADDNSC_STATUS_OK
                                          ? YADDNSC_STATUS_INTERNAL_ERROR
                                          : error.status;
        instance->error_storage = std::move(error.message);
        write_error(out_error, status, instance->error_storage, error.retry_after_seconds);
        return status;
    } catch (const ConfigParseError& e) {
        write_error(out_error, YADDNSC_STATUS_INVALID_CONFIG, copy_bounded(e.what(), "invalid driver configuration"),
                    0);
        return YADDNSC_STATUS_INVALID_CONFIG;
    } catch (const std::exception& e) {
        write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, copy_bounded(e.what(), "unknown exception during update"),
                    0);
        return YADDNSC_STATUS_INTERNAL_ERROR;
    } catch (...) {
        write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, copy_bounded({}, "unknown exception during update"), 0);
        return YADDNSC_STATUS_INTERNAL_ERROR;
    }
}

inline yaddnsc_status validate_driver(yaddnsc_driver* driver, yaddnsc_string driver_param_json,
                                      yaddnsc_error* out_error) noexcept {
    if (driver == nullptr) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "driver must not be null", 0);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    if (!yaddnsc_string_is_valid(driver_param_json)) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "driver_param_json contains an invalid view", 0);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }

    // Validate on the live instance the host created: the create → validate →
    // destroy pairing guarantees the plugin code stays mapped, and the
    // instance carries the error-storage backing for out_error.
    auto* instance = reinterpret_cast<DriverInstance*>(driver);  // NOLINT
    try {
        Result result = instance->driver->validate(to_view(driver_param_json));
        if (result.has_value()) {
            return YADDNSC_STATUS_OK;
        }

        Error& error = result.error();
        const yaddnsc_status status = !yaddnsc_status_is_valid(error.status) || error.status == YADDNSC_STATUS_OK
                                          ? YADDNSC_STATUS_INTERNAL_ERROR
                                          : error.status;
        instance->error_storage = std::move(error.message);
        write_error(out_error, status, instance->error_storage, error.retry_after_seconds);
        return status;
    } catch (const ConfigParseError& e) {
        write_error(out_error, YADDNSC_STATUS_INVALID_CONFIG, copy_bounded(e.what(), "invalid driver configuration"),
                    0);
        return YADDNSC_STATUS_INVALID_CONFIG;
    } catch (const std::exception& e) {
        write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR,
                    copy_bounded(e.what(), "unknown exception during validate"), 0);
        return YADDNSC_STATUS_INTERNAL_ERROR;
    } catch (...) {
        write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, copy_bounded({}, "unknown exception during validate"), 0);
        return YADDNSC_STATUS_INTERNAL_ERROR;
    }
}

}  // namespace detail

}  // namespace yaddnsc::sdk

/* ── Logging macros ───────────────────────────────────────────────────────*/

#define YADDNSC_SDK_LOG(svc, level, ...)                                                                             \
    [&](const char* yaddnsc_log_function) {                                                                          \
        ::yaddnsc::sdk::detail::log_lazy(                                                                            \
            [&]() { return static_cast<yaddnsc_log_level>(level); },                                                 \
            [&]() { return (svc); },                                                                                 \
            [&](const yaddnsc_host_services* yaddnsc_log_services, yaddnsc_log_level yaddnsc_log_level_value) {      \
                ::yaddnsc::sdk::detail::log_message(yaddnsc_log_services, yaddnsc_log_level_value, __FILE__,         \
                                                    __LINE__, yaddnsc_log_function, __VA_ARGS__);                    \
            });                                                                                                      \
    }(__FUNCTION__)
#define YADDNSC_SDK_LOG_TRACE(svc, ...) YADDNSC_SDK_LOG(svc, YADDNSC_LOG_TRACE, __VA_ARGS__)
#define YADDNSC_SDK_LOG_DEBUG(svc, ...) YADDNSC_SDK_LOG(svc, YADDNSC_LOG_DEBUG, __VA_ARGS__)
#define YADDNSC_SDK_LOG_INFO(svc, ...) YADDNSC_SDK_LOG(svc, YADDNSC_LOG_INFO, __VA_ARGS__)
#define YADDNSC_SDK_LOG_WARN(svc, ...) YADDNSC_SDK_LOG(svc, YADDNSC_LOG_WARN, __VA_ARGS__)
#define YADDNSC_SDK_LOG_ERROR(svc, ...) YADDNSC_SDK_LOG(svc, YADDNSC_LOG_ERROR, __VA_ARGS__)

/* ── Driver definition macro ──────────────────────────────────────────────*/

/// Emit the four required C entry points, the optional validate entry, and
/// the static descriptor for a driver.
///
/// Usage in a driver plugin:
/// @code{.cpp}
///   class MyDriver final : public yaddnsc::sdk::Driver { ... };
///   YADDNSC_DEFINE_DRIVER(MyDriver, "my_driver", "description", "Author",
///                         "1.0.0", YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA, 0)
/// @endcode
#define YADDNSC_DEFINE_DRIVER(DriverClass, driver_name, driver_description, driver_author, driver_version,             \
                              driver_capabilities, minimum_abi_minor)                                                  \
    namespace {                                                                                                        \
    static_assert((minimum_abi_minor) >= 0 && (minimum_abi_minor) <= YADDNSC_DRIVER_ABI_MINOR);                        \
    constexpr yaddnsc_driver_descriptor YADDNSC_SDK_DESCRIPTOR = {                                                     \
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
        *out_descriptor = &YADDNSC_SDK_DESCRIPTOR;                                                                     \
        return YADDNSC_STATUS_OK;                                                                                      \
    }                                                                                                                  \
                                                                                                                       \
    extern "C" YADDNSC_SDK_EXPORT yaddnsc_status yaddnsc_driver_create(                                                \
        const yaddnsc_host_services* services, yaddnsc_driver** out_driver, yaddnsc_error* out_error) {                \
        return ::yaddnsc::sdk::detail::create_driver<DriverClass, minimum_abi_minor>(services, out_driver, out_error); \
    }                                                                                                                  \
                                                                                                                       \
    extern "C" YADDNSC_SDK_EXPORT void yaddnsc_driver_destroy(yaddnsc_driver* driver) {                                \
        ::yaddnsc::sdk::detail::destroy_driver(driver);                                                                \
    }                                                                                                                  \
                                                                                                                       \
    extern "C" YADDNSC_SDK_EXPORT yaddnsc_status yaddnsc_driver_update(                                                \
        yaddnsc_driver* driver, const yaddnsc_update_request* request, yaddnsc_error* out_error) {                     \
        return ::yaddnsc::sdk::detail::update_driver(driver, request, out_error);                                      \
    }                                                                                                                  \
                                                                                                                       \
    extern "C" YADDNSC_SDK_EXPORT yaddnsc_status yaddnsc_driver_validate(                                              \
        yaddnsc_driver* driver, yaddnsc_string driver_param_json, yaddnsc_error* out_error) {                          \
        return ::yaddnsc::sdk::detail::validate_driver(driver, driver_param_json, out_error);                          \
    }

#endif  // YADDNSC_SDK_DRIVER_HPP
