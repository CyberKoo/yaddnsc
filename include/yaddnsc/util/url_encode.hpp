//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_UTIL_URL_ENCODE_HPP
#define YADDNSC_UTIL_URL_ENCODE_HPP

/// Percent-encoding (RFC 3986 §2.1) — the single implementation shared by the
/// host (Uri::url_encode) and driver plugins (yaddnsc/sdk/url_encode.hpp).

#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace yaddnsc::util {

/// Percent-encode a string per RFC 3986 §2.1.
///
/// Unreserved characters (A-Z, a-z, 0-9, '-', '.', '_', '~') are passed
/// through; all other bytes are encoded as "%XX" (uppercase hex).
///
/// When @p encode_slash is false, '/' is preserved instead of being encoded
/// as "%2F". This is needed for the canonical URI in AWS SigV4 signing,
/// where each path segment is encoded separately and '/' is the delimiter.
[[nodiscard]] inline std::string url_encode(std::string_view input, bool encode_slash = true) {
    constexpr auto HEX_CHARS = std::to_array("0123456789ABCDEF");

    std::string result;
    result.reserve(input.size() * 3);

    for (auto const c: input) {
        auto const uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || uc == '-' || uc == '.' || uc == '_' || uc == '~') {
            // unreserved character (RFC 3986 §2.3)
            result += c;
        } else if (c == '/' && !encode_slash) {
            result += c;
        } else {
            result += '%';
            result += HEX_CHARS[uc >> 4];
            result += HEX_CHARS[uc & 0xF];
        }
    }

    return result;
}

} // namespace yaddnsc::util

#endif // YADDNSC_UTIL_URL_ENCODE_HPP
