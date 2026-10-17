#ifndef YADDNSC_UTIL_URL_DECODE_HPP
#define YADDNSC_UTIL_URL_DECODE_HPP

/// Percent-decoding (RFC 3986 §2.1) — the single implementation shared by the
/// host; counterpart to <yaddnsc/util/url_encode.hpp>.

#include <array>
#include <charconv>
#include <string>
#include <string_view>
#include <system_error>

namespace yaddnsc::util {

/// Percent-decode a string per RFC 3986 §2.1.
/// Each "%XX" sequence is replaced with the corresponding byte.
/// Malformed sequences (e.g. "%GG", trailing "%") are preserved as-is.
/// Note: '+' is NOT decoded as space; that convention belongs to
/// application/x-www-form-urlencoded and is left to the caller.
[[nodiscard]] inline std::string url_decode(std::string_view input) {
    std::string result;
    result.reserve(input.size());

    for (std::size_t i = 0; i < input.size(); ++i) {
        if (auto const c = input[i]; c == '%' && i + 2 < input.size()) {
            auto const hex_pair = std::array{input[i + 1], input[i + 2]};
            unsigned int val{};
            // from_chars reports success on a partial parse ("%4G" parses
            // '4' and stops), so the consumed range must cover both digits —
            // otherwise the sequence is malformed and preserved as-is.
            if (auto [p, ec] = std::from_chars(hex_pair.data(), hex_pair.data() + hex_pair.size(), val, 16);
                ec == std::errc{} && p == hex_pair.data() + hex_pair.size()) {
                result += static_cast<char>(val);
                i += 2;
            } else {
                // Malformed percent-encoding – keep as-is
                result += c;
            }
        } else {
            result += c;
        }
    }

    return result;
}

}  // namespace yaddnsc::util

#endif  // YADDNSC_UTIL_URL_DECODE_HPP
