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
    void ltrim(std::string &s);

    void rtrim(std::string &s);

    void trim(std::string &s);

    std::string ltrimCopy(std::string s);

    std::string rtrimCopy(std::string s);

    std::string trimCopy(std::string s);

    std::string strToLower(std::string s);

    std::vector<std::string_view> split(const std::string &s, const std::string &delims);

    std::vector<std::string_view> split(std::string_view str, std::string_view delims);

    void replace(std::string &str, const std::map<std::string_view, std::string_view> &replace_list);

    std::string replaceCopy(std::string_view str, const std::map<std::string_view, std::string_view> &replace_list);
}

#endif //YADDNSC_STRING_UTIL_H
