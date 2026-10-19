#ifndef YADDNSC_SDK_UPSTREAM_FAILURE_HPP
#define YADDNSC_SDK_UPSTREAM_FAILURE_HPP

/// Upstream rejection classification and retry hints shared by both driver modes.
#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <system_error>

#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/sdk/http_date.hpp>
#include <yaddnsc/util/string_util.hpp>

namespace yaddnsc::sdk::detail {

/// First Retry-After relative to a fixed sample; past/invalid values yield zero.
template<typename Response>
[[nodiscard]] inline std::uint32_t retry_after_seconds(const Response& response,
                                                       std::chrono::sys_seconds now) noexcept {
    for (const auto& header : response.headers) {
        if (!util::iequals(header.name, "retry-after"))
            continue;
        const auto value = util::trim(header.value);
        std::uint32_t seconds = 0;
        const char* const begin = value.data();
        const char* const end = begin + value.size();
        const auto [ptr, ec] = std::from_chars(begin, end, seconds);
        if (ec == std::errc{} && ptr == end)
            return seconds;
        if (ec == std::errc::result_out_of_range && ptr == end)
            return std::numeric_limits<std::uint32_t>::max();
        const auto date = parse_http_date(value);
        if (!date || *date <= now)
            return 0;
        const auto delta = (*date - now).count();
        if (delta >= static_cast<std::chrono::seconds::rep>(std::numeric_limits<std::uint32_t>::max()))
            return std::numeric_limits<std::uint32_t>::max();
        return static_cast<std::uint32_t>(delta);
    }
    return 0;
}

template<typename Response>
[[nodiscard]] inline std::uint32_t retry_after_seconds(const Response& response) noexcept {
    return retry_after_seconds(response,
                               std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now()));
}

struct UpstreamFailure {
    yaddnsc_status status = YADDNSC_STATUS_UPSTREAM_REJECTED;
    std::uint32_t retry_after_seconds = 0;
};

/// Classify an already rejected response; 429 preserves Retry-After.
template<typename Response>
[[nodiscard]] inline UpstreamFailure classify_upstream_failure(const Response& response) noexcept {
    if (response.status_code == 401 || response.status_code == 403)
        return {YADDNSC_STATUS_AUTHENTICATION_FAILED, 0};
    if (response.status_code == 429)
        return {YADDNSC_STATUS_RATE_LIMITED, retry_after_seconds(response)};
    return {};
}

[[nodiscard]] constexpr bool is_transient_status(std::uint32_t status_code) noexcept {
    return status_code == 429 || (status_code >= 500 && status_code < 600);
}

}  // namespace yaddnsc::sdk::detail

#endif  // YADDNSC_SDK_UPSTREAM_FAILURE_HPP
