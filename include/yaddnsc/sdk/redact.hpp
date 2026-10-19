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
#include <optional>
#include <string>
#include <string_view>

#include <glaze/glaze.hpp>
#include <yaddnsc/util/string_util.hpp>
#include <yaddnsc/util/url_decode.hpp>

namespace yaddnsc::sdk::redact {

/// Whether an HTTP header key carries credentials (case-insensitive).
[[nodiscard]] inline bool is_sensitive_header(std::string_view key) {
    static constexpr std::string_view SENSITIVE_HEADERS[] = {
        "authorization", "proxy-authorization", "cookie",      "x-api-key",
        "x-auth-token",  "x-access-token",      "x-api-token", "x-amz-security-token",
    };
    const auto lower = util::to_lower_copy(key);
    return std::ranges::find(SENSITIVE_HEADERS, lower) != std::end(SENSITIVE_HEADERS);
}

/// Whether a parameter key (form body, URL query, JSON key) carries
/// credentials. Exact match first, then a suffix fallback so that
/// future drivers using *_token / *_secret / *_password / *_key
/// parameter names are covered automatically.
[[nodiscard]] inline bool is_sensitive_param(std::string_view key) {
    static constexpr std::string_view SENSITIVE_PARAMS[] = {
        "token",
        "api_key",
        "apikey",
        "secretapikey",
        "key",
        "auth",
        "secret",
        "client_secret",
        "api_secret",
        "access_key_secret",
        "secret_access_key",
        "password",
        "passwd",
        "signature",
    };
    const auto lower = util::to_lower_copy(key);
    if (std::ranges::find(SENSITIVE_PARAMS, lower) != std::end(SENSITIVE_PARAMS)) {
        return true;
    }
    return lower.ends_with("_token") || lower.ends_with("_secret") || lower.ends_with("_password") ||
           lower.ends_with("_key");
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

namespace detail {

inline void redact_json(glz::generic& value) {
    if (value.is_object()) {
        for (auto& [key, child] : value.get_object()) {
            if (is_sensitive_param(key)) {
                child = std::string("***");
            } else {
                redact_json(child);
            }
        }
    } else if (value.is_array()) {
        for (auto& child : value.get_array()) {
            redact_json(child);
        }
    }
}

/// Percent-decode @p key, then accept it only when every resulting byte is a
/// legal key character. Malformed escapes survive the decode as literal '%'
/// or a stray digit, and both fail the character check, so a bad key still
/// hides the whole input instead of being classified from raw bytes. '+' is
/// not a key character either way, so the form convention cannot smuggle a
/// space past this gate.
[[nodiscard]] inline std::optional<std::string> decode_key(std::string_view key) {
    std::string decoded = util::url_decode(key);
    if (decoded.empty())
        return std::nullopt;
    if (!std::ranges::all_of(decoded, is_key_char))
        return std::nullopt;
    return decoded;
}

}  // namespace detail

/// Parse form/query pairs using '&' as the only separator. Commas, quotes,
/// braces and newlines belong to a value, including a secret value.
/// Percent-encoded keys are decoded before classification. Malformed keys
/// or pairs hide the whole input; redaction never falls back to raw input.
[[nodiscard]] inline std::string redact_form(std::string_view body) {
    std::string result;
    while (!body.empty()) {
        const auto end = body.find('&');
        const auto pair = body.substr(0, end);
        const auto equals = pair.find('=');
        if (equals == std::string_view::npos)
            return "***";
        const auto key = detail::decode_key(pair.substr(0, equals));
        if (!key)
            return "***";
        result.append(pair.substr(0, equals + 1));
        result.append(is_sensitive_param(*key) ? "***" : pair.substr(equals + 1));
        if (end == std::string_view::npos)
            break;
        result += '&';
        body.remove_prefix(end + 1);
    }
    return result;
}

/// Parse JSON before recursively redacting sensitive keys, including nested
/// objects/arrays and escaped keys. Invalid or unrecognised content is hidden.
/// Non-JSON input is accepted only as a form/query sequence of key=value pairs.
/// JSON output is compact and may differ in whitespace/key order from input.
[[nodiscard]] inline std::string redact_body(std::string_view body, std::string_view content_type = {}) {
    if (body.empty())
        return {};
    const auto first = body.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return "***";
    const auto media_type = util::to_lower_copy(content_type.substr(0, content_type.find(';')));
    const bool json = media_type == "application/json" || media_type.ends_with("+json");
    if (json || (media_type.empty() && (body[first] == '{' || body[first] == '[' || body[first] == '"'))) {
        glz::generic value;
        if (glz::read<glz::opts_validate{}>(value, body))
            return "***";
        if (!value.is_object() && !value.is_array())
            return "***";
        detail::redact_json(value);
        const auto result = glz::write_json(value);
        return result ? *result : "***";
    }
    if (!media_type.empty() && media_type != "application/x-www-form-urlencoded")
        return "***";
    return redact_form(body);
}

/// Redact the query and hide fragments (never sent to the server). Invalid
/// query syntax hides the whole query rather than exposing a credential.
[[nodiscard]] inline std::string redact_url_query(std::string_view path) {
    const auto fragment = path.find('#');
    path = path.substr(0, fragment);
    const auto q = path.find('?');
    if (q == std::string_view::npos)
        return std::string(path);
    return std::string(path.substr(0, q + 1)) + redact_form(path.substr(q + 1));
}

/// Maximum payload bytes one log line may carry. Bodies past this budget are
/// cut down before they reach a log call so that a large upstream response
/// cannot flood the log.
inline constexpr std::size_t LOG_BODY_BUDGET = 512;

/// Stands in for the bytes dropped once a body reaches LOG_BODY_BUDGET.
inline constexpr std::string_view LOG_BODY_TRUNCATION_MARKER = "...";

/// Return @p body capped at LOG_BODY_BUDGET bytes, marked as truncated when
/// content was cut. Wrap every payload body logged by a driver in this.
[[nodiscard]] inline std::string truncate_for_log(std::string_view body) {
    if (body.size() <= LOG_BODY_BUDGET) {
        return std::string(body);
    }
    std::string out;
    out.reserve(LOG_BODY_BUDGET);
    out.append(body.data(), LOG_BODY_BUDGET - LOG_BODY_TRUNCATION_MARKER.size());
    out.append(LOG_BODY_TRUNCATION_MARKER);
    return out;
}

}  // namespace yaddnsc::sdk::redact

#endif  // YADDNSC_SDK_REDACT_HPP
