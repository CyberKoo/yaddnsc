//
// Created by Kotarou on 2022/4/7.
//
#include "string_util.h"

#include <algorithm>

std::string StringUtil::strToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::vector<std::string_view> StringUtil::split(const std::string &s, const std::string &delim) {
    return StringUtil::split(std::string_view(s), std::string_view(delim));
}

std::vector<std::string_view> StringUtil::split(std::string_view str, std::string_view delims = " ") {
    std::vector<std::string_view> output;
    size_t first = 0;

    while (first < str.size()) {
        const auto second = str.find(delims, first);

        if (first != second) {
            output.emplace_back(str.substr(first, second - first));
        }

        if (second == std::string_view::npos) {
            break;
        }

        first = second + delims.size();
    }

    return output;
}

// from https://stackoverflow.com/questions/216823/whats-the-best-way-to-trim-stdstring
// trim from start (in place)
void StringUtil::ltrim(std::string &s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](int ch) {
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
    StringUtil::ltrim(s);
    StringUtil::rtrim(s);
}

// trim from start (copying)
std::string StringUtil::ltrimCopy(std::string s) {
    StringUtil::ltrim(s);
    return s;
}

// trim from end (copying)
std::string StringUtil::rtrimCopy(std::string s) {
    StringUtil::rtrim(s);
    return s;
}

// trim from both ends (copying)
std::string StringUtil::trimCopy(std::string s) {
    StringUtil::trim(s);
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
StringUtil::replaceCopy(std::string_view str, const std::map<std::string_view, std::string_view> &replace_list) {
    auto s = std::string(str);
    StringUtil::replace(s, replace_list);

    return s;
}