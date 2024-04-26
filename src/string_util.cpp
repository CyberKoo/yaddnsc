//
// Created by Kotarou on 2022/4/7.
//
#include "string_util.h"

#include <algorithm>

void StringUtil::to_upper(std::string &_str) {
    str_transform(_str, &::toupper);
}

void StringUtil::to_lower(std::string &_str) {
    str_transform(_str, &::tolower);
}

// from https://stackoverflow.com/questions/216823/whats-the-best-way-to-trim-stdstring
// trim from start (in place)
void StringUtil::ltrim(std::string &s) {
    s.erase(s.begin(), std::ranges::find_if(s, [](int ch) {
        return !std::isspace(ch);
    }));
}

// trim from end (in place)
void StringUtil::rtrim(std::string &s) {
    s.erase(std::find_if(s.rbegin(), s.rend(), [](int ch) {
        return !std::isspace(ch);
    }).base(), s.end());
}

// trim from both ends (in place)
void StringUtil::trim(std::string &s) {
    ltrim(s);
    rtrim(s);
}

// trim from start (copying)
std::string StringUtil::ltrim_copy(std::string s) {
    ltrim(s);
    return s;
}

// trim from end (copying)
std::string StringUtil::rtrim_copy(std::string s) {
    rtrim(s);
    return s;
}

// trim from both ends (copying)
std::string StringUtil::trim_copy(std::string s) {
    trim(s);
    return s;
}

void StringUtil::replace(std::string &str, const std::map<std::string_view, std::string_view> &replace_list) {
    for (const auto &[target, new_content]: replace_list) {
        auto pos = str.find(target);
        while (pos != std::string::npos) {
            str.replace(pos, target.length(), new_content);
            pos = str.find(target);
        }
    }
}

std::string
StringUtil::replace_copy(std::string_view str, const std::map<std::string_view, std::string_view> &replace_list) {
    auto s = std::string(str);
    StringUtil::replace(s, replace_list);

    return s;
}
