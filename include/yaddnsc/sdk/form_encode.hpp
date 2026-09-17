//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_SDK_FORM_ENCODE_HPP
#define YADDNSC_SDK_FORM_ENCODE_HPP

/// Percent-encoding helpers for driver plugins.
///
/// application/x-www-form-urlencoded rules: unreserved characters pass
/// through, spaces become '+', everything else is %XX (uppercase hex).

#include <array>
#include <map>
#include <string>
#include <string_view>

namespace yaddnsc::sdk {

namespace detail {

/// Characters that pass through unencoded.
///
/// Beyond the RFC 3986 unreserved set, query-safe sub-delimiters and the
/// characters httplib historically let through in query components are kept
/// verbatim — DNSPod and similar APIs expect literal '@', '/', ':' etc.
[[nodiscard]] inline bool is_query_safe(const unsigned char c) noexcept {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
        c == '_' || c == '~') {
        return true;
    }
    switch (c) {
        case '!':
        case '$':
        case '\'':
        case '(':
        case ')':
        case '*':
        case ',':
        case ';':
        case ':':
        case '@':
        case '/':
        case '?':
            return true;
        default:
            return false;
    }
}

inline constexpr std::array<char, 16> HEX{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

} // namespace detail

/// Encode a single value (or name) per form semantics.
[[nodiscard]] inline std::string encode_form_component(const std::string_view value) {
    std::string out;
    out.reserve(value.size());

    for (const char raw: value) {
        const auto c = static_cast<unsigned char>(raw);
        if (c == ' ') {
            out += '+';
        } else if (c == '+') {
            out += "%2B";
        } else if (detail::is_query_safe(c)) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += detail::HEX[c >> 4];
            out += detail::HEX[c & 0x0F];
        }
    }
    return out;
}

/// Encode a parameter map as "k1=v1&k2=v2" (keys and values encoded,
/// pairs joined by '&').
[[nodiscard]] inline std::string encode_form(const std::multimap<std::string, std::string> &params) {
    std::string out;
    bool first = true;
    for (const auto &[key, value]: params) {
        if (!first) {
            out += '&';
        }
        first = false;
        out += encode_form_component(key);
        out += '=';
        out += encode_form_component(value);
    }
    return out;
}

} // namespace yaddnsc::sdk

#endif // YADDNSC_SDK_FORM_ENCODE_HPP
