//
// Created by Kotarou on 2026/6/18.
//

#ifndef YADDNSC_HTTP_FMT_H
#define YADDNSC_HTTP_FMT_H

#include "fmt.hpp"
#include "http_type.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

/// Redaction helpers for log output.
///
/// API credentials (tokens, secrets, passwords) must never be written to
/// logs in plain text.  All redaction rules live here so that every log
/// site that formats an HttpRequest (or a URL) is protected by default.
namespace Utils::Redact {

    /// Lowercase a string (ASCII).
    [[nodiscard]] inline std::string to_lower(std::string_view s) noexcept {
        std::string out(s);
        for (auto &ch: out) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return out;
    }

    /// Whether an HTTP header key carries credentials (case-insensitive).
    [[nodiscard]] inline bool is_sensitive_header(std::string_view key) noexcept {
        static constexpr std::string_view SENSITIVE_HEADERS[] = {
            "authorization", "proxy-authorization", "cookie",
            "x-api-key", "x-auth-token", "x-access-token", "x-api-token",
            "x-amz-security-token",
        };
        const auto lower = to_lower(key);
        return std::ranges::find(SENSITIVE_HEADERS, lower) != std::end(SENSITIVE_HEADERS);
    }

    /// Whether a parameter key (form body, URL query, JSON key) carries
    /// credentials.  Exact match first, then a suffix fallback so that
    /// future drivers using *_token / *_secret / *_password / *_key
    /// parameter names are covered automatically.
    [[nodiscard]] inline bool is_sensitive_param(std::string_view key) noexcept {
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
    [[nodiscard]] inline std::string redact_header(std::string_view key, std::string_view value) noexcept {
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
    /// ("\"key\": value") shapes with a single scan.  Non-sensitive keys
    /// and values are preserved verbatim.  This is a best-effort textual
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
    /// carries the API token in the URL).  Non-query paths pass through.
    [[nodiscard]] inline std::string redact_url_query(std::string_view path) {
        const auto q = path.find('?');
        if (q == std::string_view::npos) {
            return std::string(path);
        }
        return std::string(path.substr(0, q + 1)) + redact_body(path.substr(q + 1));
    }

} // namespace Utils::Redact

/// fmt / std::format formatter for HttpRequest
/// (also covers DriverRequest, which is a type alias for HttpRequest).

#ifdef YADDNSC_USE_STD_FORMAT
template<>
struct std::formatter<HttpRequest> {
#else
    template<>
    struct fmt::formatter<HttpRequest> {

#endif

    /// Convert an HttpMethod enum value to its string representation.
    static std::string_view to_string(const HttpMethod type) {
        switch (type) {
            case HttpMethod::GET:
                return "GET";
            case HttpMethod::POST:
                return "POST";
            case HttpMethod::PUT:
                return "PUT";
            case HttpMethod::PATCH:
                return "PATCH";
            case HttpMethod::DEL:
                return "DELETE";
            case HttpMethod::HEAD:
                return "HEAD";
            case HttpMethod::OPTIONS:
                return "OPTIONS";
        }

        std::unreachable();
    }

    /// Format a key-value map range into a human-readable string, redacting
    /// credential-bearing header values.
    /// @param first  Iterator to the first key-value pair.
    /// @param last   Past-the-end iterator.
    /// @return       String like "key1=val1; key2=***".
    template<typename Iter>
    [[nodiscard]] static std::string format_map(Iter first, Iter last) {
        std::string buf;

        for (auto it = first; it != last; ++it) {
            buf.append(it->first);
            buf.append("=");
            buf.append(Utils::Redact::redact_header(it->first, it->second));
            buf.append("; ");
        }

        if (buf.size() >= 2) {
            buf.erase(buf.size() - 2);
        }

        return buf;
    }

    /// Parse the format specification.
    /// HttpRequest accepts no custom format options, so this always
    /// returns the end-of-spec iterator.
    /// @return  Iterator past the format spec (always ctx.begin()).
    static constexpr auto parse(format_parse_context &ctx) -> decltype(ctx.begin()) {
        return ctx.begin();
    }

    /// Format an HttpRequest into the output context.
    /// Renders as: HttpRequest(body="...", content_type="...", method="...", header="...")
    /// Sensitive header values and body/query parameters are redacted.
    /// @param request  The HTTP request to format.
    /// @param ctx      The format output context.
    /// @return         Iterator past the last written character.
    template<typename FormatContext>
    auto format(const HttpRequest &request, FormatContext &ctx) const -> decltype(ctx.out()) {
        const auto &body = request.body.value_or("");

        return fmt::format_to(
            ctx.out(),
            R"(HttpRequest(body="{}", content_type="{}", method="{}", header="{}"))",
            Utils::Redact::redact_body(body), request.content_type, to_string(request.method),
            format_map(request.headers.begin(), request.headers.end())
        );
    }
};

#endif //YADDNSC_HTTP_FMT_H
