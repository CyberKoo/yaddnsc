//
// Unit tests for the pure helpers in <yaddnsc/sdk/driver.hpp> that are not
// exercised by the C ABI round-trip tests:
//
//   - method_name            method mapping plus the out-of-range fallback
//   - parse_http_date        IMF-fixdate parsing and its rejection rules
//   - retry_after_seconds    delay-seconds, HTTP-date and out-of-range forms
//   - log_message            null service/log tables are dropped
//   - parse_config / parse_response  glz error mapping to ConfigParseError
//
// The ABI entry points themselves (create/update/validate_driver) are covered
// by test/plugin and test/sdk_consumer.
// =============================================================================

#include <chrono>
#include <cstdint>
#include <ctime>
#include <format>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <yaddnsc/sdk/driver.hpp>

using yaddnsc::sdk::HttpHeader;
using yaddnsc::sdk::HttpHeaderView;
using yaddnsc::sdk::HttpResponse;
using yaddnsc::sdk::Method;

namespace {

HttpResponse response_with(std::initializer_list<std::pair<std::string, std::string>> headers) {
    HttpResponse response;
    response.status_code = 503;
    for (const auto& [name, value] : headers) {
        response.headers.push_back(HttpHeaderView{name, value});
    }
    return response;
}

// =============================================================================
// method_name
// =============================================================================

TEST(SdkMethodName, KnownMethods_MapToTheirNames) {
    EXPECT_EQ(yaddnsc::sdk::method_name(Method::GET), "GET");
    EXPECT_EQ(yaddnsc::sdk::method_name(Method::POST), "POST");
    EXPECT_EQ(yaddnsc::sdk::method_name(Method::PUT), "PUT");
    EXPECT_EQ(yaddnsc::sdk::method_name(Method::PATCH), "PATCH");
    EXPECT_EQ(yaddnsc::sdk::method_name(Method::DELETE), "DELETE");
    EXPECT_EQ(yaddnsc::sdk::method_name(Method::HEAD), "HEAD");
    EXPECT_EQ(yaddnsc::sdk::method_name(Method::OPTIONS), "OPTIONS");
}

TEST(SdkMethodName, OutOfRangeValue_FallsBackToGet) {
    EXPECT_EQ(yaddnsc::sdk::method_name(static_cast<Method>(9999)), "GET");
}

// =============================================================================
// parse_http_date
// =============================================================================

TEST(SdkParseHttpDate, IbmFixdate_ParsesToEpochSeconds) {
    // Sun, 06 Nov 1994 08:49:37 GMT == 784111777
    const auto parsed = yaddnsc::sdk::detail::parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->time_since_epoch().count(), 784111777);
}

TEST(SdkParseHttpDate, LowercaseGmtAndPaddedDay_AreAccepted) {
    const auto parsed = yaddnsc::sdk::detail::parse_http_date("Sun, 06 nov 1994 08:49:37 gmt");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->time_since_epoch().count(), 784111777);
}

TEST(SdkParseHttpDate, SingleDigitDay_IsAccepted) {
    const auto parsed = yaddnsc::sdk::detail::parse_http_date("Sun, 6 Nov 1994 08:49:37 GMT");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->time_since_epoch().count(), 784111777);
}

TEST(SdkParseHttpDate, NoComma_ReturnsNullopt) {
    EXPECT_FALSE(yaddnsc::sdk::detail::parse_http_date("06 Nov 1994 08:49:37 GMT").has_value());
}

TEST(SdkParseHttpDate, CommaAtEnd_ReturnsNullopt) {
    EXPECT_FALSE(yaddnsc::sdk::detail::parse_http_date("Sun,").has_value());
}

TEST(SdkParseHttpDate, EmptyText_ReturnsNullopt) {
    EXPECT_FALSE(yaddnsc::sdk::detail::parse_http_date("").has_value());
}

TEST(SdkParseHttpDate, NonNumericDay_ReturnsNullopt) {
    EXPECT_FALSE(yaddnsc::sdk::detail::parse_http_date("Sun, xx Nov 1994 08:49:37 GMT").has_value());
}

TEST(SdkParseHttpDate, DayZero_ReturnsNullopt) {
    EXPECT_FALSE(yaddnsc::sdk::detail::parse_http_date("Sun, 00 Nov 1994 08:49:37 GMT").has_value());
}

TEST(SdkParseHttpDate, DayAbove31_ReturnsNullopt) {
    EXPECT_FALSE(yaddnsc::sdk::detail::parse_http_date("Sun, 32 Nov 1994 08:49:37 GMT").has_value());
}

TEST(SdkParseHttpDate, MissingSpaceAfterDay_ReturnsNullopt) {
    EXPECT_FALSE(yaddnsc::sdk::detail::parse_http_date("Sun, 06Nov 1994 08:49:37 GMT").has_value());
}

TEST(SdkParseHttpDate, TextEndsAfterDay_ReturnsNullopt) {
    EXPECT_FALSE(yaddnsc::sdk::detail::parse_http_date("Sun, 06").has_value());
}

TEST(SdkParseHttpDate, MonthShorterThan3Chars_ReturnsNullopt) {
    EXPECT_FALSE(yaddnsc::sdk::detail::parse_http_date("Sun, 06 No 1994 08:49:37 GMT").has_value());
}

TEST(SdkParseHttpDate, UnknownMonth_ReturnsNullopt) {
    EXPECT_FALSE(yaddnsc::sdk::detail::parse_http_date("Sun, 06 Xxx 1994 08:49:37 GMT").has_value());
}

TEST(SdkParseHttpDate, MissingSpaceAfterMonth_ReturnsNullopt) {
    EXPECT_FALSE(yaddnsc::sdk::detail::parse_http_date("Sun, 06 Nov1994 08:49:37 GMT").has_value());
}

TEST(SdkParseHttpDate, NonNumericYear_ReturnsNullopt) {
    EXPECT_FALSE(yaddnsc::sdk::detail::parse_http_date("Sun, 06 Nov xxxx 08:49:37 GMT").has_value());
}

TEST(SdkParseHttpDate, YearBefore1970_ReturnsNullopt) {
    EXPECT_FALSE(yaddnsc::sdk::detail::parse_http_date("Sun, 06 Nov 1969 08:49:37 GMT").has_value());
}

TEST(SdkParseHttpDate, YearAbove9999_ReturnsNullopt) {
    EXPECT_FALSE(yaddnsc::sdk::detail::parse_http_date("Sun, 06 Nov 10000 08:49:37 GMT").has_value());
}

TEST(SdkParseHttpDate, TruncatedTimeField_ReturnsNullopt) {
    EXPECT_FALSE(yaddnsc::sdk::detail::parse_http_date("Sun, 06 Nov 1994 08:4 GMT").has_value());
}

TEST(SdkParseHttpDate, NonNumericTime_ReturnsNullopt) {
    EXPECT_FALSE(yaddnsc::sdk::detail::parse_http_date("Sun, 06 Nov 1994 aa:49:37 GMT").has_value());
}

TEST(SdkParseHttpDate, HourAbove23_ReturnsNullopt) {
    EXPECT_FALSE(yaddnsc::sdk::detail::parse_http_date("Sun, 06 Nov 1994 24:49:37 GMT").has_value());
}

TEST(SdkParseHttpDate, MinuteAbove59_ReturnsNullopt) {
    EXPECT_FALSE(yaddnsc::sdk::detail::parse_http_date("Sun, 06 Nov 1994 08:60:37 GMT").has_value());
}

TEST(SdkParseHttpDate, SecondAbove60_ReturnsNullopt) {
    EXPECT_FALSE(yaddnsc::sdk::detail::parse_http_date("Sun, 06 Nov 1994 08:49:61 GMT").has_value());
}

TEST(SdkParseHttpDate, NonGmtZone_ReturnsNullopt) {
    EXPECT_FALSE(yaddnsc::sdk::detail::parse_http_date("Sun, 06 Nov 1994 08:49:37 UTC").has_value());
}

TEST(SdkParseHttpDate, AllTwelveMonths_AreAccepted) {
    for (int month = 1; month <= 12; ++month) {
        static constexpr std::string_view names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        std::string text = "Sun, 06 " + std::string(names[month - 1]) + " 1994 08:49:37 GMT";
        EXPECT_TRUE(yaddnsc::sdk::detail::parse_http_date(text).has_value()) << text;
    }
}

TEST(SdkParseHttpDate, LeapDay_IsAccepted) {
    const auto parsed = yaddnsc::sdk::detail::parse_http_date("Thu, 29 Feb 1996 00:00:00 GMT");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_GT(parsed->time_since_epoch().count(), 0);
}

// =============================================================================
// retry_after_seconds
// =============================================================================

TEST(SdkRetryAfter, NoRetryAfterHeader_ReturnsZero) {
    EXPECT_EQ(yaddnsc::sdk::detail::retry_after_seconds(response_with({{"Content-Type", "text/plain"}})), 0u);
}

TEST(SdkRetryAfter, EmptyHeaderList_ReturnsZero) {
    EXPECT_EQ(yaddnsc::sdk::detail::retry_after_seconds(response_with({})), 0u);
}

TEST(SdkRetryAfter, DelaySeconds_IsReturnedVerbatim) {
    EXPECT_EQ(yaddnsc::sdk::detail::retry_after_seconds(response_with({{"Retry-After", "120"}})), 120u);
}

TEST(SdkRetryAfter, DelaySecondsWithWhitespace_IsTrimmed) {
    EXPECT_EQ(yaddnsc::sdk::detail::retry_after_seconds(response_with({{"Retry-After", "  30  "}})), 30u);
}

TEST(SdkRetryAfter, HeaderNameIsMatchedCaseInsensitively) {
    EXPECT_EQ(yaddnsc::sdk::detail::retry_after_seconds(response_with({{"retry-after", "45"}})), 45u);
}

TEST(SdkRetryAfter, FirstMatchingHeaderWins) {
    EXPECT_EQ(yaddnsc::sdk::detail::retry_after_seconds(response_with({{"Retry-After", "10"}, {"Retry-After", "20"}})), 10u);
}

TEST(SdkRetryAfter, NonRetryAfterHeaderBeforeMatch_IsSkipped) {
    EXPECT_EQ(yaddnsc::sdk::detail::retry_after_seconds(response_with({{"X-Other", "999"}, {"Retry-After", "7"}})), 7u);
}

TEST(SdkRetryAfter, DelaySecondsAboveUint32Max_Saturates) {
    EXPECT_EQ(yaddnsc::sdk::detail::retry_after_seconds(response_with({{"Retry-After", "99999999999999"}})),
              std::numeric_limits<std::uint32_t>::max());
}

TEST(SdkRetryAfter, UnparsableValue_ReturnsZero) {
    EXPECT_EQ(yaddnsc::sdk::detail::retry_after_seconds(response_with({{"Retry-After", "soon"}})), 0u);
}

TEST(SdkParseHttpDateOnly, NegativeDelaySeconds_FallsBackToDateParse) {
    // "-5" is not a valid delay-seconds token; it is not a date either.
    EXPECT_EQ(yaddnsc::sdk::detail::retry_after_seconds(response_with({{"Retry-After", "-5"}})), 0u);
}

TEST(SdkRetryAfter, PastHttpDate_ReturnsZero) {
    EXPECT_EQ(yaddnsc::sdk::detail::retry_after_seconds(response_with({{"Retry-After", "Sun, 06 Nov 1994 08:49:37 GMT"}})), 0u);
}

/// Render a sys_time as an IMF-fixdate, used to build Retry-After dates
/// relative to the current clock.
std::string imf_fixdate(const std::chrono::sys_seconds& when) {
    static constexpr std::string_view days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static constexpr std::string_view names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const auto as_time_t = static_cast<std::time_t>(when.time_since_epoch().count());
    std::tm utc{};
    ::gmtime_r(&as_time_t, &utc);
    return std::format("{}, {:02} {} {:04} {:02}:{:02}:{:02} GMT", days[static_cast<size_t>(utc.tm_wday)], utc.tm_mday,
                       names[static_cast<size_t>(utc.tm_mon)], utc.tm_year + 1900, utc.tm_hour, utc.tm_min,
                       utc.tm_sec);
}

TEST(SdkRetryAfter, FutureHttpDateWithinRange_ReturnsPositiveDelay) {
    const auto now = std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto delay =
        yaddnsc::sdk::detail::retry_after_seconds(response_with({{"Retry-After", imf_fixdate(now + std::chrono::hours(2))}}));
    // Truncation toward now, plus slack for the clock ticking during the call.
    EXPECT_GE(delay, 7100u);
    EXPECT_LE(delay, 7200u);
}

TEST(SdkRetryAfter, FutureHttpDateBeyondUint32Range_Saturates) {
    // Year 9999 is further away than 2^32-1 seconds, so the delta saturates.
    EXPECT_EQ(
        yaddnsc::sdk::detail::retry_after_seconds(response_with({{"Retry-After", "Sun, 06 Nov 9999 08:49:37 GMT"}})),
        std::numeric_limits<std::uint32_t>::max());
}

}  // namespace
