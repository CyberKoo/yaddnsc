//
// Percent-encoding for the net::http client domain.
//
#include "http_client/form_encode.h"

#include <array>

namespace net::http {

namespace {

/// Characters that pass through unencoded.
///
/// Beyond the RFC 3986 unreserved set, query-safe sub-delimiters and the
/// characters httplib historically let through in query components are kept
/// verbatim — DNSPod and similar APIs expect literal '@', '/', ':' etc.
[[nodiscard]] bool is_query_safe(const unsigned char c) noexcept {
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

constexpr std::array<char, 16> HEX{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

} // namespace

std::string encode_form_component(const std::string_view value) {
    std::string out;
    out.reserve(value.size());

    for (const unsigned char c: value) {
        if (c == ' ') {
            out += '+';
        } else if (c == '+') {
            out += "%2B";
        } else if (is_query_safe(c)) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += HEX[c >> 4];
            out += HEX[c & 0x0F];
        }
    }
    return out;
}

std::string encode_form(const std::multimap<std::string, std::string>& params) {
    std::string out;
    bool first = true;
    for (const auto& [key, value] : params) {
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

}  // namespace net::http
