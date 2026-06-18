//
// Created by Kotarou on 2022/4/7.
//
#include "string_util.h"

#include <algorithm>
#include <ranges>
#include <cctype>

void StringUtil::to_upper(std::string &_str) {
    std::ranges::transform(_str, _str.begin(), [](const unsigned char c) { return ::toupper(c); });
}

void StringUtil::to_lower(std::string &_str) {
    std::ranges::transform(_str, _str.begin(), [](const unsigned char c) { return ::tolower(c); });
}

// from https://stackoverflow.com/questions/216823/whats-the-best-way-to-trim-stdstring
// trim from start (in place)
void StringUtil::ltrim(std::string &s) {
    s.erase(s.begin(), std::ranges::find_if(s, [](const unsigned char ch) {
        return !std::isspace(ch);
    }));
}

// trim from end (in place)
void StringUtil::rtrim(std::string &s) {
    const auto it = std::ranges::find_if(s | std::views::reverse, [](const unsigned char ch) {
        return !std::isspace(ch);
    });
    s.erase(it.base(), s.end());
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
