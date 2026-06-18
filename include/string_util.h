//
// Created by Kotarou on 2022/4/7.
//

#ifndef YADDNSC_STRING_UTIL_H
#define YADDNSC_STRING_UTIL_H

#include <vector>
#include <string>
#include <ranges>
#include <string_view>

namespace StringUtil {
    void to_lower(std::string &);

    void to_upper(std::string &);

    void ltrim(std::string &);

    void rtrim(std::string &);

    void trim(std::string &);

    std::string ltrim_copy(std::string);

    std::string rtrim_copy(std::string);

    std::string trim_copy(std::string);

    template<typename T>
    requires std::ranges::input_range<const T> &&
        requires { typename std::ranges::range_value_t<const T>::first_type; } &&
        requires { typename std::ranges::range_value_t<const T>::second_type; } &&
        std::is_constructible_v<std::string_view, typename std::ranges::range_value_t<const T>::first_type> &&
        std::is_constructible_v<std::string_view, typename std::ranges::range_value_t<const T>::second_type>
    void replace(std::string &str, const T &replace_list) {
        for (const auto &[target, new_content] : replace_list) {
            std::string_view target_sv(target);
            std::string_view new_content_sv(new_content);
            auto pos = str.find(target_sv);
            while (pos != std::string::npos) {
                str.replace(pos, target_sv.length(), new_content_sv);
                pos = str.find(target_sv, pos + new_content_sv.length());
            }
        }
    }

    template<typename T>
    requires std::ranges::input_range<const T> &&
        requires { typename std::ranges::range_value_t<const T>::first_type; } &&
        requires { typename std::ranges::range_value_t<const T>::second_type; } &&
        std::is_constructible_v<std::string_view, typename std::ranges::range_value_t<const T>::first_type> &&
        std::is_constructible_v<std::string_view, typename std::ranges::range_value_t<const T>::second_type>
    std::string replace_copy(std::string_view str, const T &replace_list) {
        auto s = std::string(str);
        replace(s, replace_list);
        return s;
    }

    template<typename T> requires std::is_constructible_v<std::string_view, T>
    std::vector<std::string> split(T &&_str, std::string_view delim = " ") {
        std::string_view str(_str);
        std::vector<std::string> output;

        for (auto part : str | std::views::split(delim)) {
            if (!part.empty()) {
                output.emplace_back(part.begin(), part.end());
            }
        }

        return output;
    }

    template<typename T> requires std::is_constructible_v<std::string, T>
    std::string to_lower_copy(T &&_str) {
        std::string new_str(_str);
        to_lower(new_str);

        return new_str;
    }

    template<typename T> requires std::is_constructible_v<std::string, T>
    std::string to_upper_copy(T &&_str) {
        std::string new_str(_str);
        to_upper(new_str);

        return new_str;
    }

    template<typename T> requires std::is_constructible_v<std::string_view, T>
    bool str_to_bool(T &&_str) {
        std::string_view s(_str);
        return s == "1" || s == "on" || s == "true" || s == "yes"
            || s == "ON" || s == "TRUE" || s == "YES";
    }
}

#endif //YADDNSC_STRING_UTIL_H
