#ifndef YADDNSC_SDK_HTTP_DATE_HPP
#define YADDNSC_SDK_HTTP_DATE_HPP

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

#include <yaddnsc/util/string_util.hpp>

namespace yaddnsc::sdk::detail {

/// Gregorian date to days since 1970-01-01 (Howard Hinnant's civil algorithm).
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
        if (util::iequals(name, months[i]))
            return static_cast<int>(i) + 1;
    }
    return 0;
}

/// IMF-fixdate; weekday is ignored. Invalid fields return nullopt; no allocation.
[[nodiscard]] inline std::optional<std::chrono::sys_seconds> parse_http_date(std::string_view text) noexcept {
    const auto comma = text.find(',');
    if (comma == std::string_view::npos || comma + 1 >= text.size())
        return std::nullopt;
    text.remove_prefix(comma + 1);
    text = util::trim(text);
    unsigned day = 0;
    const auto [day_end, day_ec] = std::from_chars(text.data(), text.data() + text.size(), day);
    if (day_ec != std::errc{} || day < 1 || day > 31)
        return std::nullopt;
    text.remove_prefix(static_cast<std::size_t>(day_end - text.data()));
    if (text.empty() || text.front() != ' ')
        return std::nullopt;
    text.remove_prefix(1);
    if (text.size() < 3)
        return std::nullopt;
    const int month = month_index(text.substr(0, 3));
    if (month == 0 || text.size() < 4 || text[3] != ' ')
        return std::nullopt;
    text.remove_prefix(4);
    unsigned year = 0;
    const auto [year_end, year_ec] = std::from_chars(text.data(), text.data() + text.size(), year);
    if (year_ec != std::errc{} || year < 1970 || year > 9999)
        return std::nullopt;
    text.remove_prefix(static_cast<std::size_t>(year_end - text.data()));
    if (text.size() < 10 || text.front() != ' ' || text[3] != ':' || text[6] != ':')
        return std::nullopt;
    text.remove_prefix(1);
    unsigned hour = 0, minute = 0, second = 0;
    const auto [hour_end, hour_ec] = std::from_chars(text.data(), text.data() + 2, hour);
    const auto [minute_end, minute_ec] = std::from_chars(text.data() + 3, text.data() + 5, minute);
    const auto [second_end, second_ec] = std::from_chars(text.data() + 6, text.data() + 8, second);
    if (hour_ec != std::errc{} || minute_ec != std::errc{} || second_ec != std::errc{} || hour_end != text.data() + 2 ||
        minute_end != text.data() + 5 || second_end != text.data() + 8 || hour > 23 || minute > 59 || second > 60)
        return std::nullopt;
    text.remove_prefix(8);
    text = util::trim(text);
    if (!util::iequals(text, "GMT"))
        return std::nullopt;
    const auto days = days_from_civil(static_cast<int>(year), static_cast<unsigned>(month), day);
    const auto secs = static_cast<std::int64_t>(days) * 86400 + static_cast<std::int64_t>(hour) * 3600 +
                      static_cast<std::int64_t>(minute) * 60 + static_cast<std::int64_t>(second);
    return std::chrono::sys_seconds{std::chrono::seconds{secs}};
}

}  // namespace yaddnsc::sdk::detail

#endif  // YADDNSC_SDK_HTTP_DATE_HPP
