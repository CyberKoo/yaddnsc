#ifndef YADDNSC_SDK_DRIVER_COMMON_HPP
#define YADDNSC_SDK_DRIVER_COMMON_HPP

/// Source-level DTOs and ABI value conversion shared by sync and async
/// drivers. No execution entry points, host internals, logging or update-tail
/// helpers; C++ values stay in the plugin. Log macros live in
/// <yaddnsc/sdk/log.hpp>; the update tail helpers in
/// <yaddnsc/sdk/run_update.hpp>.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <glaze/glaze.hpp>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/sdk/format.hpp>

#define YADDNSC_SDK_EXPORT __attribute__((visibility("default")))

namespace yaddnsc::sdk {

/// Invalid configuration aborts an operation and becomes INVALID_CONFIG at ABI entry.
class ConfigParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Error {
    yaddnsc_status status = YADDNSC_STATUS_INTERNAL_ERROR;
    std::string message;
    uint32_t retry_after_seconds = 0;
};

using Result = std::expected<void, Error>;

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

/// Engaged body means send a body (possibly empty); nullopt means no body.
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

/// Borrowed response; views survive until the synchronous update entry returns,
/// across exchanges. Copying this value does not extend host arena lifetime.
struct HttpResponse {
    uint32_t status_code = 0;
    std::vector<HttpHeaderView> headers;
    std::string_view body;
};

struct HttpError {
    yaddnsc_status status = YADDNSC_STATUS_NETWORK_ERROR;
    std::string message;
    uint32_t retry_after_seconds = 0;
};

using ExchangeResult = std::expected<HttpResponse, HttpError>;

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

/// Views borrow host input for one update; async instances retain owned copies.
struct UpdateRequest {
    std::string_view ip_address;
    std::string_view record_type;
    std::string_view domain;
    std::string_view subdomain;
    std::string_view fqdn;
    std::string_view driver_param_json;
};

namespace detail {

template<std::size_t N>
[[nodiscard]] consteval yaddnsc_string descriptor_string(const char (&value)[N]) {
    static_assert(N > 0);
    if (value[N - 1] != '\0')
        throw "descriptor strings must be NUL-terminated";
    return {value, N - 1};
}

[[nodiscard]] inline yaddnsc_host_services copy_services(const yaddnsc_host_services& source) noexcept {
    yaddnsc_host_services result{};
    std::memcpy(&result, &source, source.struct_size < sizeof(result) ? source.struct_size : sizeof(result));
    result.struct_size =
        static_cast<uint32_t>(source.struct_size < sizeof(result) ? source.struct_size : sizeof(result));
    return result;
}

inline constexpr uint16_t HOST_SERVICES_V1_1_MINOR = 1;

/// Source must be the base subobject of a complete advertised 1.1 container.
/// Copy only its known prefix and query field, never an unknown provider tail.
[[nodiscard]] inline yaddnsc_host_services_v1_1 copy_services_v1_1(const yaddnsc_host_services& source) noexcept {
    yaddnsc_host_services_v1_1 result{};
    result.base = copy_services(source);
    if (yaddnsc_struct_has_field(source.struct_size, YADDNSC_HOST_SERVICES_V1_1_MIN_SIZE) &&
        yaddnsc_abi_provides(source.abi_major, source.abi_minor, YADDNSC_DRIVER_ABI_MAJOR, HOST_SERVICES_V1_1_MINOR)) {
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

/// No-allocation diagnostic storage, valid until the next call on this thread.
[[nodiscard]] inline std::string_view copy_bounded(std::string_view message, std::string_view fallback) noexcept {
    thread_local std::array<char, 512> storage{};
    const auto source = message.data() == nullptr ? fallback : message;
    const auto size = source.size() < storage.size() - 1 ? source.size() : storage.size() - 1;
    if (size != 0)
        std::memcpy(storage.data(), source.data(), size);
    storage[size] = '\0';
    return {storage.data(), size};
}

/// Honor caller capacity without touching an unknown tail. No allocation.
inline void write_error(yaddnsc_error* out_error, yaddnsc_status status, std::string_view message,
                        uint32_t retry_after_seconds) noexcept {
    if (out_error == nullptr || out_error->struct_size < YADDNSC_ERROR_MIN_SIZE)
        return;
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
        !yaddnsc_http_header_array_is_valid(response.headers, response.header_count))
        return false;
    for (size_t i = 0; i < response.header_count; ++i) {
        if (!yaddnsc_http_header_is_valid(response.headers[i]))
            return false;
    }
    return true;
}

[[nodiscard]] inline bool valid_update_request(const yaddnsc_update_request& request) noexcept {
    return request.struct_size >= YADDNSC_UPDATE_REQUEST_MIN_SIZE && yaddnsc_string_is_valid(request.ip_address) &&
           yaddnsc_string_is_valid(request.record_type) && yaddnsc_string_is_valid(request.domain) &&
           yaddnsc_string_is_valid(request.subdomain) && yaddnsc_string_is_valid(request.fqdn) &&
           yaddnsc_bytes_is_valid(request.driver_param_json);
}

/// Terminal results never carry PENDING. Storage retains error bytes through
/// the next entry; allocation failures propagate to the enclosing ABI firewall.
inline yaddnsc_status translate_result(Result result, std::string& storage, yaddnsc_error* out_error) {
    if (result)
        return YADDNSC_STATUS_OK;
    auto& error = result.error();
    const auto status = !yaddnsc_status_is_valid(error.status) || error.status == YADDNSC_STATUS_OK
                            ? YADDNSC_STATUS_INTERNAL_ERROR
                            : error.status;
    storage = std::move(error.message);
    write_error(out_error, status, storage, error.retry_after_seconds);
    return status;
}

}  // namespace detail

/// Source-level JSON helpers; exception translation belongs to each ABI entry.
class DriverHelpers {
protected:
    template<typename T>
    [[nodiscard]] static T parse_config(std::string_view json) {
        T value{};
        const auto ec = glz::read<glz::opts{.error_on_missing_keys = true}>(value, json, glz::context{});
        if (ec == glz::error_code::none)
            return value;
        throw ConfigParseError(fmt::format("Driver configuration parse error: {}", glz::format_error(ec)));
    }

    template<typename T>
    [[nodiscard]] static std::optional<T> parse_response(std::string_view body) {
        T value{};
        const auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(value, body, glz::context{});
        if (ec == glz::error_code::none)
            return value;
        return std::nullopt;
    }

    /// Reject a required parameter that is present but empty.
    static void require_non_empty(std::string_view name, std::string_view value) {
        if (value.empty()) {
            throw ConfigParseError(fmt::format("Required parameter '{}' must not be empty", name));
        }
    }

    /// Reject an optional parameter that was provided with an empty value. The
    /// parameter itself stays optional; only an empty value is rejected, so
    /// the message matches the required case. A template so a plain string
    /// argument cannot convert to `optional` and turn the call ambiguous.
    template<typename T>
    static void require_non_empty(std::string_view name, const std::optional<T>& value) {
        if (value && value->empty()) {
            throw ConfigParseError(fmt::format("Required parameter '{}' must not be empty", name));
        }
    }

    /// Reject a negative optional numeric parameter.
    static void optional_non_negative(std::string_view name, const std::optional<int>& value) {
        if (value && *value < 0) {
            throw ConfigParseError(fmt::format("Parameter '{}' must not be negative", name));
        }
    }

    /// Validate driver parameters with the driver's own parser. The parser
    /// reports an unacceptable document by throwing ConfigParseError; the ABI
    /// entry turns that into YADDNSC_STATUS_INVALID_CONFIG and keeps the
    /// message, so validate and update reject the same documents.
    template<typename ParseParams>
    [[nodiscard]] static Result validate_via(std::string_view driver_param_json, ParseParams&& parse_params) {
        [[maybe_unused]] const auto parsed = std::forward<ParseParams>(parse_params)(driver_param_json);
        return {};
    }
};

}  // namespace yaddnsc::sdk

#endif  // YADDNSC_SDK_DRIVER_COMMON_HPP
