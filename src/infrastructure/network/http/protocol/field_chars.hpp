//
// Shared HTTP field-syntax predicates for the net::http layer.
//
// is_token / is_field_value validate header field-name (RFC 9110 §5.1)
// and field-value characters. They are used by both the request builder
// (http/wire_request.cpp) and the response parser (http/protocol/exchange.cpp).
//

#ifndef YADDNSC_HTTP_CLIENT_PROTOCOL_FIELD_CHARS_HPP
#define YADDNSC_HTTP_CLIENT_PROTOCOL_FIELD_CHARS_HPP

#include <cctype>
#include <string_view>

namespace net::http::protocol {

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

}  // namespace net::http::protocol

#endif  // YADDNSC_HTTP_CLIENT_PROTOCOL_FIELD_CHARS_HPP
