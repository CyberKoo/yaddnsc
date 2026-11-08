#ifndef YADDNSC_SDK_RUN_UPDATE_HPP
#define YADDNSC_SDK_RUN_UPDATE_HPP

/// Shared request logging and response handling for synchronous and
/// asynchronous updates. Both mode headers re-export these helpers.
///
/// The response helpers below carry the skeleton every bundled driver shares:
/// the response body trace, the transient-status rejection and the fallback
/// for a response without provider detail. A driver keeps only its own
/// provider-specific branches.

#include <cstdint>
#include <functional>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

#include <concepts>

#include <yaddnsc/sdk/driver_common.hpp>
#include <yaddnsc/sdk/format.hpp>
#include <yaddnsc/sdk/log.hpp>
#include <yaddnsc/sdk/redact.hpp>
#include <yaddnsc/sdk/upstream_failure.hpp>

namespace yaddnsc::sdk {

/// Credential-redacted request log representation, with bounded body output.
[[nodiscard]] inline std::string format_request(const HttpRequest& request) {
    std::string headers;
    for (const auto& [name, value] : request.headers) {
        headers.append(name);
        headers.append("=");
        headers.append(redact::redact_header(name, value));
        headers.append("; ");
    }
    if (headers.size() >= 2)
        headers.erase(headers.size() - 2);
    return fmt::format(R"(Request(body="{}", content_type="{}", method="{}", url="{}", header="{}"))",
                       redact::truncate_for_log(redact::redact_body(request.body.value_or(""), request.content_type)),
                       request.content_type, method_name(request.method), redact::redact_url_query(request.url),
                       headers);
}

namespace detail {

/// Keep rendering inside the level check, preserving the driver's call site.
template<typename Services>
inline void log_update_request(const Services& services, const UpdateRequest& params, std::string_view driver_name,
                               const HttpRequest& request, std::source_location location) {
    log_lazy([&] { return YADDNSC_LOG_DEBUG; }, [&] { return services; },
             [&](const auto* target, yaddnsc_log_level level) {
                 log_message(target, level, location.file_name(), static_cast<int32_t>(location.line()),
                             location.function_name(),
                             "Domain \"{}\" ({}) received DNS record update request from driver '{}', {}", params.fqdn,
                             params.record_type, driver_name, format_request(request));
             });
}

/// Non-coroutine update tail. Response storage stays with the caller throughout
/// checker invocation; detailed Result errors and bool classification both survive.
template<typename Services, typename Response, typename CheckResponse>
[[nodiscard]] inline Result finish_update(const Services& services, const UpdateRequest& params, Response& response,
                                          CheckResponse&& check_response, std::source_location location) {
    if (!response) {
        log_lazy([&] { return YADDNSC_LOG_WARN; }, [&] { return services; },
                 [&](const auto* target, yaddnsc_log_level level) {
                     log_message(target, level, location.file_name(), static_cast<int32_t>(location.line()),
                                 location.function_name(), "Domain \"{}\" ({}) update failed: {}", params.fqdn,
                                 params.record_type, response.error().message);
                 });
        return std::unexpected(
            Error{response.error().status, response.error().message, response.error().retry_after_seconds});
    }
    auto checked = std::invoke(std::forward<CheckResponse>(check_response), *response, Services{services});
    if (!checked) {
        log_lazy(
            [&] { return detail::is_transient_status(response->status_code) ? YADDNSC_LOG_WARN : YADDNSC_LOG_ERROR; },
            [&] { return services; },
            [&](const auto* target, yaddnsc_log_level level) {
                log_message(target, level, location.file_name(), static_cast<int32_t>(location.line()),
                            location.function_name(), "Domain \"{}\" ({}) update rejected by upstream", params.fqdn,
                            params.record_type);
            });
        if constexpr (std::same_as<decltype(checked), Result>) {
            return checked;
        }
        const auto failure = classify_upstream_failure(*response);
        return std::unexpected(Error{
            failure.status, fmt::format("Domain {} ({}) update rejected by upstream", params.fqdn, params.record_type),
            failure.retry_after_seconds});
    }
    return {};
}

}  // namespace detail

/// Log the upstream response body at TRACE level. The body is truncated to the
/// log budget and formatted only when that level is enabled.
template<typename Services, typename Response>
inline void log_response_body(const Services& services, const Response& response,
                              std::source_location location = std::source_location::current()) {
    detail::log_lazy([&] { return YADDNSC_LOG_TRACE; }, [&] { return services; },
                     [&](const auto* target, yaddnsc_log_level level) {
                         detail::log_message(target, level, location.file_name(), static_cast<int32_t>(location.line()),
                                             location.function_name(), "Got {} from server",
                                             redact::truncate_for_log(response.body));
                     });
}

/// The message for a response that carries no provider error detail.
template<typename Response>
[[nodiscard]] inline std::string rejection_message(std::string_view subject, const Response& response) {
    return fmt::format("{} failed with HTTP status {}", subject, response.status_code);
}

/// The rejection to report for a response the driver refused: the upstream
/// status classification (401/403 become AUTHENTICATION_FAILED, 429 becomes
/// RATE_LIMITED and keeps its Retry-After hint) and the provider's own error
/// message.
template<typename Response>
[[nodiscard]] inline Error upstream_rejection(const Response& response, std::string message) {
    const auto failure = detail::classify_upstream_failure(response);
    return Error{failure.status, std::move(message), failure.retry_after_seconds};
}

/// Reject a transient upstream status (429 or 5xx). The next update cycle
/// retries it, so the event is a WARN rather than an ERROR. Returns the
/// rejection to report, or nullopt when the status is not transient.
template<typename Services, typename Response>
[[nodiscard]] inline std::optional<Error> transient_rejection(
    const Services& services, const Response& response, std::string_view subject,
    std::source_location location = std::source_location::current()) {
    if (!detail::is_transient_status(response.status_code)) {
        return std::nullopt;
    }
    detail::log_lazy([&] { return YADDNSC_LOG_WARN; }, [&] { return services; },
                     [&](const auto* target, yaddnsc_log_level level) {
                         detail::log_message(target, level, location.file_name(), static_cast<int32_t>(location.line()),
                                             location.function_name(), "{} failed with HTTP status {}: {}", subject,
                                             response.status_code, redact::truncate_for_log(response.body));
                     });
    return upstream_rejection(response, rejection_message(subject, response));
}

/// Log and report a response that carries no provider error detail: the
/// upstream body is logged at ERROR level and the rejection names the HTTP
/// status. Operator action is required for such a rejection.
template<typename Services, typename Response>
[[nodiscard]] inline Error status_rejection(const Services& services, const Response& response,
                                            std::string_view subject,
                                            std::source_location location = std::source_location::current()) {
    detail::log_lazy([&] { return YADDNSC_LOG_ERROR; }, [&] { return services; },
                     [&](const auto* target, yaddnsc_log_level level) {
                         if (response.body.empty()) {
                             detail::log_message(target, level, location.file_name(),
                                                 static_cast<int32_t>(location.line()), location.function_name(),
                                                 "{} failed with HTTP status {}", subject, response.status_code);
                             return;
                         }
                         detail::log_message(target, level, location.file_name(), static_cast<int32_t>(location.line()),
                                             location.function_name(), "{} failed with HTTP status {}: {}", subject,
                                             response.status_code, redact::truncate_for_log(response.body));
                     });
    return upstream_rejection(response, rejection_message(subject, response));
}

}  // namespace yaddnsc::sdk

#endif  // YADDNSC_SDK_RUN_UPDATE_HPP
