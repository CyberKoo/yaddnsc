//
// Created by Kotarou on 2022/4/7.
//

#ifndef YADDNSC_STRING_UTIL_H
#define YADDNSC_STRING_UTIL_H

#include <map>
#include <vector>
#include <string>
#include <functional>
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

    void replace(std::string &, const std::map<std::string_view, std::string_view> &);

    std::string replace_copy(std::string_view, const std::map<std::string_view, std::string_view> &);

    template<typename T> requires std::is_constructible_v<std::string_view, T>
    std::vector<std::string> split(T &&_str, std::string_view delim = " ") {
        std::string_view str(_str);
        std::vector<std::string> output;
        size_t first = 0;
        size_t second;

        while (first < str.size() && (second = str.find(delim, first)) != std::string_view::npos) {
            if (first != second) {
                output.emplace_back(str.substr(first, second - first));
            }

            first = second + delim.size();
        }

        return output;
    }

    template<typename T> requires std::is_constructible_v<std::string_view, T>
    void str_transform(T &_str, const std::function<int(int)> &func) {
        for (auto &c: _str) {
            c = func(c);
        }
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

    template<typename T> requires std::is_constructible_v<std::string, T>
    bool str_to_bool(T &&_str) {
        std::string new_str(to_lower_copy(_str));
        return new_str == "1" || new_str == "on" || new_str == "true" || new_str == "yes";
    }
}

#endif //YADDNSC_STRING_UTIL_H
