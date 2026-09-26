//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_SDK_REDACT_HPP
#define YADDNSC_SDK_REDACT_HPP

/// Redaction helpers for driver log output.
///
/// API credentials (tokens, secrets, passwords) must never be written to
/// logs in plain text. Every log site that formats an HttpRequest (or a URL)
/// is protected by default. This header is the SDK-side implementation; the
/// host's own redaction (config show) lives in
/// src/infrastructure/config/config.cpp.

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace yaddnsc::sdk::redact {

/// Lowercase a string (ASCII).
[[nodiscard]] inline std::string to_lower(std::string_view s) {
    std::string out(s);
    for (auto &ch: out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

/// Whether an HTTP header key carries credentials (case-insensitive).
[[nodiscard]] inline bool is_sensitive_header(std::string_view key) {
    static constexpr std::string_view SENSITIVE_HEADERS[] = {
        "authorization", "proxy-authorization", "cookie",
        "x-api-key", "x-auth-token", "x-access-token", "x-api-token",
        "x-amz-security-token",
    };
    const auto lower = to_lower(key);
    return std::ranges::find(SENSITIVE_HEADERS, lower) != std::end(SENSITIVE_HEADERS);
}

/// Whether a parameter key (form body, URL query, JSON key) carries
/// credentials. Exact match first, then a suffix fallback so that
/// future drivers using *_token / *_secret / *_password / *_key
/// parameter names are covered automatically.
[[nodiscard]] inline bool is_sensitive_param(std::string_view key) {
    static constexpr std::string_view SENSITIVE_PARAMS[] = {
        "token", "api_key", "apikey", "auth", "secret", "client_secret", "api_secret",
        "access_key_secret", "secret_access_key", "password", "passwd",
        "signature",
    };
    const auto lower = to_lower(key);
    if (std::ranges::find(SENSITIVE_PARAMS, lower) != std::end(SENSITIVE_PARAMS)) {
        return true;
    }
    return lower.ends_with("_token") || lower.ends_with("_secret") ||
           lower.ends_with("_password") || lower.ends_with("_key");
}

/// Redact a header value if its key is sensitive; otherwise pass through.
[[nodiscard]] inline std::string redact_header(std::string_view key, std::string_view value) {
    return is_sensitive_header(key) ? std::string("***") : std::string(value);
}

/// First character of a form/query key.
[[nodiscard]] inline bool is_key_start_char(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

/// Character that may appear inside a key.
[[nodiscard]] inline bool is_key_char(char c) noexcept {
    return is_key_start_char(c) || (c >= '0' && c <= '9') || c == '.' || c == '-';
}

/// Whether a key may start at @p pos (beginning of input or right after
/// a separator: '&', ',', ' ', '\n', '\r').
[[nodiscard]] inline bool is_key_position(std::string_view s, std::size_t pos) noexcept {
    if (pos == 0) {
        return true;
    }
    const char prev = s[pos - 1];
    return prev == '&' || prev == ',' || prev == ' ' || prev == '\n' || prev == '\r';
}

/// Redact the values of sensitive parameters in a body or query string.
///
/// Handles both form-encoding ("key=value&...") and JSON
/// ("\"key\": value") shapes with a single scan. Non-sensitive keys
/// and values are preserved verbatim. This is a best-effort textual
/// redaction for logging purposes only — it is not a parser.
[[nodiscard]] inline std::string redact_body(std::string_view body) {
    std::string result;
    result.reserve(body.size());

    const auto value_end = [&body](std::size_t start) {
        std::size_t pos = start;
        while (pos < body.size() && body[pos] != '&' && body[pos] != ',' &&
               body[pos] != '}' && body[pos] != '\n' && body[pos] != '\r') {
            ++pos;
        }
        return pos;
    };

    std::size_t pos = 0;
    while (pos < body.size()) {
        // ── JSON shape: '"key"' followed by ':' ──
        if (body[pos] == '"') {
            const auto key_end = body.find('"', pos + 1);
            if (key_end == std::string_view::npos) {
                result.append(body.substr(pos));
                break;
            }
            std::size_t colon = key_end + 1;
            while (colon < body.size() && (body[colon] == ' ' || body[colon] == '\t')) {
                ++colon;
            }
            if (colon < body.size() && body[colon] == ':') {
                const auto key = body.substr(pos + 1, key_end - pos - 1);
                if (is_sensitive_param(key)) {
                    // Copy '"key":' + whitespace, then replace the value.
                    std::size_t val_start = colon + 1;
                    while (val_start < body.size() &&
                           (body[val_start] == ' ' || body[val_start] == '\t')) {
                        ++val_start;
                    }
                    result.append(body.substr(pos, val_start - pos));
                    result.append("***");
                    pos = value_end(val_start);
                    continue;
                }
            }
            result.append(body.substr(pos, key_end + 1 - pos));
            pos = key_end + 1;
            continue;
        }

        // ── Form shape: 'key=' at a key position ──
        if (is_key_position(body, pos)) {
            std::size_t k = pos;
            while (k < body.size() && is_key_char(body[k])) {
                ++k;
            }
            if (k < body.size() && body[k] == '=') {
                const auto key = body.substr(pos, k - pos);
                if (is_sensitive_param(key)) {
                    const auto val_start = k + 1;
                    result.append(body.substr(pos, val_start - pos));  // "key="
                    result.append("***");
                    pos = value_end(val_start);
                    continue;
                }
            }
        }

        result += body[pos];
        ++pos;
    }
    return result;
}

/// Redact sensitive query parameters in a request path (e.g. DuckDNS
/// carries the API token in the URL). Non-query paths pass through.
[[nodiscard]] inline std::string redact_url_query(std::string_view path) {
    const auto q = path.find('?');
    if (q == std::string_view::npos) {
        return std::string(path);
    }
    return std::string(path.substr(0, q + 1)) + redact_body(path.substr(q + 1));
}

} // namespace yaddnsc::sdk::redact

#endif // YADDNSC_SDK_REDACT_HPP
