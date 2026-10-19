//
// http — field-syntax predicates shared by the request builder and the
// response parser.
//
// is_token / is_field_value validate header field-name (RFC 9110 §5.1) and
// field-value characters; the list helpers read a comma-separated Connection
// field (RFC 9110 §5.6.1).
//

#ifndef YADDNSC_INFRASTRUCTURE_HTTP_PROTOCOL_FIELD_CHARS_HPP
#define YADDNSC_INFRASTRUCTURE_HTTP_PROTOCOL_FIELD_CHARS_HPP

#include <cctype>
#include <string_view>

#include "support/string_util.hpp"  // IWYU pragma: keep — the StringUtil namespace alias lives here

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

/// RFC 9110 §5.6.1: a list field is a non-empty comma-separated token list.
[[nodiscard]] inline bool valid_token_list(const std::string_view value) noexcept {
    std::size_t pos = 0;
    do {
        const auto comma = value.find(',', pos);
        const auto token = StringUtil::trim(value.substr(pos, comma == std::string_view::npos ? comma : comma - pos));
        if (!is_token(token)) {
            return false;
        }
        if (comma == std::string_view::npos) {
            return true;
        }
        pos = comma + 1;
    } while (pos < value.size());
    return false;
}

/// Whether a list field names `wanted`, compared case-insensitively.
[[nodiscard]] inline bool has_token(const std::string_view value, const std::string_view wanted) noexcept {
    std::size_t pos = 0;
    while (pos < value.size()) {
        const auto comma = value.find(',', pos);
        if (StringUtil::iequals(
                StringUtil::trim(value.substr(pos, comma == std::string_view::npos ? comma : comma - pos)), wanted)) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        pos = comma + 1;
    }
    return false;
}

}  // namespace http::protocol

#endif  // YADDNSC_INFRASTRUCTURE_HTTP_PROTOCOL_FIELD_CHARS_HPP
