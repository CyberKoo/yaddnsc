//
// http — field-syntax predicates shared by the request builder and the
// response parser.
//
// is_token / is_field_value validate header field-name (RFC 9110 §5.1) and
// field-value characters. Carried over from the legacy HTTP client, where they
// lived in an anonymous-namespace-adjacent helper header.
//

#ifndef YADDNSC_CORO_HTTP_PROTOCOL_FIELD_CHARS_HPP
#define YADDNSC_CORO_HTTP_PROTOCOL_FIELD_CHARS_HPP

#include <cctype>
#include <string_view>

namespace http::protocol {

/// RFC 9110 §5.1: a field name must be a valid token.
[[nodiscard]] inline bool is_token(const std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    for (const auto ch : value) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || ch == '!' || ch == '#' || ch == '$' || ch == '%' || ch == '&' || ch == '\'' ||
            ch == '*' || ch == '+' || ch == '-' || ch == '.' || ch == '^' || ch == '_' || ch == '`' || ch == '|' ||
            ch == '~') {
            continue;
        }
        return false;
    }
    return true;
}

/// RFC 9110 §5.5: a field value must not contain CR/LF or control
/// characters other than horizontal tab.
[[nodiscard]] inline bool is_field_value(const std::string_view value) noexcept {
    for (const auto ch : value) {
        const auto c = static_cast<unsigned char>(ch);
        if (c != '\t' && (c < 0x20 || c == 0x7f)) {
            return false;
        }
    }
    return true;
}

}  // namespace http::protocol

#endif  // YADDNSC_CORO_HTTP_PROTOCOL_FIELD_CHARS_HPP
