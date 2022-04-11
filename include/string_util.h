//
// Created by Kotarou on 2022/4/7.
//

#ifndef YADDNSC_STRING_UTIL_H
#define YADDNSC_STRING_UTIL_H

#include <map>
#include <vector>
#include <string>
#include <string_view>

namespace StringUtil {
    void ltrim(std::string &);

    void rtrim(std::string &);

    void trim(std::string &);

    bool str_to_bool(std::string_view);

    std::string ltrim_copy(std::string);

    std::string rtrim_copy(std::string);

    std::string trim_copy(std::string);

    void to_lower(std::string &);

    void to_upper(std::string &);

    std::string to_lower(std::string);

    std::string to_upper(std::string);

    std::vector<std::string> split(std::string_view str, std::string_view delim = " ");

    void replace(std::string &, const std::map<std::string_view, std::string_view> &);

    std::string replace_copy(std::string_view, const std::map<std::string_view, std::string_view> &);
}

#endif //YADDNSC_STRING_UTIL_H
