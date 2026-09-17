//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_SDK_URL_ENCODE_HPP
#define YADDNSC_SDK_URL_ENCODE_HPP

/// Percent-encoding for driver plugins (RFC 3986 §2.1), used by cloud API
/// signing schemes (Alibaba Cloud RPC, AWS SigV4 canonical URIs).

#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace yaddnsc::sdk {

/// Percent-encode a string per RFC 3986 §2.1.
///
/// Unreserved characters (A-Z, a-z, 0-9, '-', '.', '_', '~') are passed
/// through; all other bytes are encoded as "%XX" (uppercase hex).
///
/// When @p encode_slash is false, '/' is preserved instead of being encoded
/// as "%2F". This is needed for the canonical URI in AWS SigV4 signing,
/// where each path segment is encoded separately and '/' is the delimiter.
[[nodiscard]] inline std::string url_encode(std::string_view input, bool encode_slash = true) noexcept {
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

} // namespace yaddnsc::sdk

#endif // YADDNSC_SDK_URL_ENCODE_HPP
