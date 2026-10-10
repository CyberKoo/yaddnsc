#include "infrastructure/uri/uri.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <yaddnsc/util/url_encode.hpp>

#include "domain/network/inet_address.h"

namespace {
/// Known scheme-to-default-port mappings.
const std::unordered_map<std::string_view, int> KNOWN_PORTS = {
    {"http", 80},
    {"https", 443},
    {"tls", 853},
};

constexpr std::string_view DEFAULT_PATH = "/";

[[nodiscard]] int lookup_default_port(std::string_view scheme) noexcept {
    auto it = KNOWN_PORTS.find(scheme);
    return it != KNOWN_PORTS.end() ? it->second : 0;
}

[[nodiscard]] bool is_default_port(std::string_view scheme, int port) noexcept {
    auto it = KNOWN_PORTS.find(scheme);
    return it != KNOWN_PORTS.end() && it->second == port;
}

[[nodiscard]] bool is_ascii_alpha(char c) noexcept {
    auto const uc = static_cast<unsigned char>(c);
    return (uc >= 'A' && uc <= 'Z') || (uc >= 'a' && uc <= 'z');
}

[[nodiscard]] bool is_ascii_digit(char c) noexcept {
    auto const uc = static_cast<unsigned char>(c);
    return uc >= '0' && uc <= '9';
}

/// RFC 3986 §3.1: scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ).
[[nodiscard]] bool is_valid_scheme(std::string_view s) noexcept {
    if (s.empty() || !is_ascii_alpha(s.front()))
        return false;
    return std::all_of(s.begin() + 1, s.end(), [](char c) {
        return is_ascii_alpha(c) || is_ascii_digit(c) || c == '+' || c == '-' || c == '.';
    });
}

/// Bytes that may never appear raw in a URI: ASCII controls, space, DEL.
/// Bytes ≥ 0x80 are tolerated (IRI convention; curl accepts them too).
[[nodiscard]] bool has_invalid_character(std::string_view s) noexcept {
    return std::any_of(s.begin(), s.end(),
                       [](char c) { auto const uc = static_cast<unsigned char>(c); return uc <= 0x20 || uc == 0x7F; });
}

/// Parse a port substring into port_out.  An empty port is legal per
/// RFC 3986 (port = *DIGIT) and leaves port_out untouched.  Non-digit
/// characters and out-of-range values are hard errors: silently dropping or
/// truncating a port previously connected callers to the wrong endpoint.
[[nodiscard]] std::expected<void, UriError> parse_port(std::string_view port_str, std::optional<int>& port_out) {
    if (port_str.empty())
        return {};
    if (!std::all_of(port_str.begin(), port_str.end(), is_ascii_digit))
        return std::unexpected(UriError::PORT_NOT_NUMERIC);
    int v{};
    // All digits were pre-checked, so the only possible failure is overflow.
    if (auto [p, ec] = std::from_chars(port_str.data(), port_str.data() + port_str.size(), v);
        ec != std::errc{} || v > 65535)
        return std::unexpected(UriError::PORT_OUT_OF_RANGE);
    port_out.emplace(v);
    return {};
}

/// Lowercase a range of characters in-place within a string.
void lowercase_range(std::string& s, std::size_t pos, std::size_t len) noexcept {
    if (len == 0)
        return;
    auto start = s.begin() + static_cast<std::ptrdiff_t>(pos);
    std::transform(start, start + static_cast<std::ptrdiff_t>(len), start,
                   [](unsigned char c) -> char { return static_cast<char>(std::tolower(c)); });
}
}  // namespace

std::string_view error_message(const UriError err) noexcept {
    switch (err) {
        case UriError::INVALID_CHARACTER:
            return "input contains a space, control character, or DEL";
        case UriError::INVALID_SCHEME:
            return "invalid scheme (must be ALPHA, then ALPHA/DIGIT/'+','-','.')";
        case UriError::EMPTY_HOST:
            return "authority present but host is empty";
        case UriError::UNCLOSED_IPV6_BRACKET:
            return "unclosed IPv6 literal bracket";
        case UriError::INVALID_IPV6_LITERAL:
            return "bracketed host is not a valid IPv6 address";
        case UriError::IPV6_TRAILING_GARBAGE:
            return "unexpected characters after ']' (expected ':port' or end of authority)";
        case UriError::PORT_NOT_NUMERIC:
            return "port contains non-digit characters";
        case UriError::PORT_OUT_OF_RANGE:
            return "port out of range (must be 0-65535)";
    }
    return "unknown URI error";
}

// ---------------------------------------------------------------------------
// Parse
// ---------------------------------------------------------------------------

std::expected<Uri, UriError> Uri::parse(std::string_view uri) {
    auto result = parse_impl(uri);
    if (result && !result->port_.has_value()) {
        // Invariant: port_ is always engaged once parse() succeeds — 0 means
        // "no port and no well-known default". Enforced here, in one place,
        // so no parse_impl exit path can publish a disengaged port_.
        result->port_.emplace(0);
    }
    return result;
}

std::expected<Uri, UriError> Uri::parse_impl(std::string_view uri) {
    Uri result{};

    if (uri.empty()) {
        return result;
    }

    // Bytes that may never appear raw in any URI component.
    if (has_invalid_character(uri)) {
        return std::unexpected(UriError::INVALID_CHARACTER);
    }

    // raw_uri_ is the sole string owner — all slices point into it.
    result.raw_uri_ = uri;

    // -------- strip fragment (#...) ---------------------------------------
    // Fragment is always the very last component per RFC 3986.
    std::size_t frag_pos = std::string_view::npos;
    if (auto const p = uri.find('#'); p != std::string_view::npos) {
        frag_pos = p;
    }

    std::string_view const u = (frag_pos != std::string_view::npos) ? uri.substr(0, frag_pos) : uri;

    // -------- scheme detection --------------------------------------------
    // Look for "://".  If found and there is at least one character before
    // it, treat the part before "://" as a scheme name.
    bool has_scheme = false;
    std::size_t authority_start = 0;  // offset where authority begins

    auto const hier_delim = u.find("://");
    if (hier_delim != std::string_view::npos && hier_delim > 0) {
        // "://" can never appear in a hostname:port authority, so a prefix
        // that is not a valid scheme is malformed input, not a bare host.
        if (!is_valid_scheme(u.substr(0, hier_delim))) {
            return std::unexpected(UriError::INVALID_SCHEME);
        }
        has_scheme = true;

        result.schema_.assign(0, hier_delim);
        // scheme is case-insensitive → lowercase in-place within raw_uri_
        lowercase_range(result.raw_uri_, 0, hier_delim);

        authority_start = hier_delim + 3;  // skip "://"
        result.body_.assign(authority_start, u.size() - authority_start);
    } else {
        // No scheme – the whole input (minus fragment) is treated as
        // authority, path, or a combination thereof.
        result.body_.assign(0, u.size());
    }

    // -------- locate path & query boundaries ------------------------------
    auto const query_pos = u.find('?', authority_start);
    auto const path_pos = u.find('/', authority_start);

    // -------- path-only inputs (no authority) -----------------------------
    // Relative/absolute paths with no scheme and no host.
    if (!has_scheme && (u.starts_with('/') || u.starts_with('.'))) {
        // The query is still a separate component in a path-only reference.
        if (auto const q = u.find('?'); q != std::string_view::npos) {
            result.path_.assign(0, q);
            result.query_string_.assign(q + 1, u.size() - q - 1);
        } else {
            result.path_.assign(0, u.size());
        }
        return result;
    }

    // -------- extract authority -------------------------------------------
    // Authority runs from authority_start up to the earliest structural
    // delimiter (path, query, or end-of-string).
    auto authority_end = u.size();
    if (path_pos != std::string_view::npos) {
        authority_end = (std::min) (authority_end, path_pos);
    }
    if (query_pos != std::string_view::npos) {
        authority_end = (std::min) (authority_end, query_pos);
    }

    auto const auth_view = u.substr(authority_start, authority_end - authority_start);
    if (auto authority = parse_authority(auth_view, result.host_, result.port_, result.is_ipv6_, authority_start);
        !authority) {
        return std::unexpected(authority.error());
    }

    // host is case-insensitive per RFC 3986 §3.2.2 → lowercase in-place
    if (!result.host_.empty()) {
        lowercase_range(result.raw_uri_, result.host_.pos, result.host_.len);
    }

    // -------- empty host ---------------------------------------------------
    // The authority was present but yielded no host. With a scheme that is
    // always a mistake ("http://", "http:///path"); without one it rejects
    // the bare ":port" form. Checked before the default-port fallback below
    // engages port_, so port_.has_value() here means "explicitly parsed".
    if (result.host_.empty() && (has_scheme || result.port_.has_value())) {
        return std::unexpected(UriError::EMPTY_HOST);
    }

    // -------- default port ------------------------------------------------
    // Only apply well-known defaults when a scheme was explicitly given.
    // Bare host:port inputs keep whatever port was (or was not) specified.
    if (!result.port_.has_value()) {
        int const fallback = has_scheme ? default_port_for(result.schema_.view(result.raw_uri_)) : 0;
        result.port_.emplace(fallback);
    }

    // -------- path --------------------------------------------------------
    if (path_pos != std::string_view::npos && (query_pos == std::string_view::npos || path_pos < query_pos)) {
        // Path runs from the first '/' up to (but not including) '?'.
        auto const path_end = (query_pos != std::string_view::npos) ? query_pos : u.size();
        result.path_.assign(path_pos, path_end - path_pos);
    }

    // -------- query string ------------------------------------------------
    if (query_pos != std::string_view::npos) {
        result.query_string_.assign(query_pos + 1, u.size() - query_pos - 1);
    }

    // -------- default path ------------------------------------------------
    // An empty path with an explicit authority (scheme + host) defaults to "/"
    // per RFC 3986 §3.3.  This is handled by get_path() to avoid storing a
    // sentinel slice for the common case.

    return result;
}

// ---------------------------------------------------------------------------
// parse_authority
// ---------------------------------------------------------------------------

std::expected<void, UriError> Uri::parse_authority(std::string_view auth, Slice& host_out, std::optional<int>& port_out,
                                                   bool& is_ipv6_out, std::size_t auth_raw_offset) {
    if (auth.empty()) {
        return {};
    }

    if (auth.starts_with('[')) {
        auto const closing = auth.find(']');
        if (closing == std::string_view::npos) {
            return std::unexpected(UriError::UNCLOSED_IPV6_BRACKET);
        }

        // IPvFuture and zone IDs (RFC 6874) are not supported — no use case
        // in this project.
        if (!domain::Inet6Address::parse(auth.substr(1, closing - 1))) {
            return std::unexpected(UriError::INVALID_IPV6_LITERAL);
        }

        host_out.assign(auth_raw_offset + 1, closing - 1);
        is_ipv6_out = true;

        if (closing + 1 == auth.size()) {
            return {};
        }
        if (auth[closing + 1] != ':') {
            return std::unexpected(UriError::IPV6_TRAILING_GARBAGE);
        }
        return parse_port(auth.substr(closing + 2), port_out);
    }

    // Bare IPv6 (e.g. ::1, 2001:db8::1)
    // Only attempt Inet6Address::parse when auth contains ':' — plain
    // hostnames and IPv4 addresses never have one, so this avoids an
    // expensive inet_pton call on every parse.
    if (auth.contains(':') && domain::Inet6Address::parse(auth)) {
        host_out.assign(auth_raw_offset, auth.size());
        is_ipv6_out = true;
        return {};
    }

    // Possibly host:port  (exactly one colon)
    auto const colon = auth.rfind(':');
    if (colon != std::string_view::npos) {
        host_out.assign(auth_raw_offset, colon);
        return parse_port(auth.substr(colon + 1), port_out);
    }

    host_out.assign(auth_raw_offset, auth.size());
    return {};
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

int Uri::default_port_for(std::string_view scheme) noexcept {
    return lookup_default_port(scheme);
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

std::string_view Uri::get_schema() const noexcept {
    return view(schema_);
}

std::string_view Uri::get_host() const noexcept {
    return view(host_);
}

std::string_view Uri::get_host_literal() const noexcept {
    if (!is_ipv6_) {
        return view(host_);
    }
    // Lazy-compute the bracketed form for IPv6 hosts.
    if (host_bracketed_cache_.empty()) {
        host_bracketed_cache_ = std::string{'['} + std::string(view(host_)) + ']';
    }
    return host_bracketed_cache_;
}

int Uri::get_port() const noexcept {
    // parse() fills port_ with either the explicit value or the default-port
    // fallback; value_or keeps this noexcept accessor safe even for a Uri
    // that did not come from parse().
    return port_.value_or(0);
}

std::string_view Uri::get_path() const noexcept {
    if (path_.empty()) {
        // Default "/" for scheme URIs with no explicit path (RFC 3986 §3.3).
        return schema_.empty() ? std::string_view{} : DEFAULT_PATH;
    }
    return view(path_);
}

std::string_view Uri::get_query_string() const noexcept {
    return view(query_string_);
}

std::string_view Uri::get_body() const noexcept {
    return view(body_);
}

std::string_view Uri::get_raw_uri() const noexcept {
    return raw_uri_;
}

std::string Uri::get_origin() const {
    auto const schema_view = view(schema_);
    auto const host_view = get_host_literal();
    // port_ is always engaged after parse() — see the postcondition in parse().
    const auto port = *port_;

    if (schema_view.empty()) {
        if (port != 0) {
            return std::string(host_view) + ':' + std::to_string(port);
        }
        return std::string(host_view);
    }
    if (is_default_port(schema_view, port)) {
        return std::string(schema_view) + "://" + std::string(host_view);
    }
    return std::string(schema_view) + "://" + std::string(host_view) + ':' + std::to_string(port);
}

std::vector<std::pair<std::string, std::string>> Uri::get_query_params(bool plus_to_space) const {
    std::vector<std::pair<std::string, std::string>> params;

    if (query_string_.empty()) {
        return params;
    }

    auto const qs = view(query_string_);

    std::size_t start = 0;
    while (start < qs.size()) {
        // Find the next '&' separator
        auto const end = qs.find('&', start);
        auto const segment = (end == std::string_view::npos) ? qs.substr(start) : qs.substr(start, end - start);

        // Skip empty segments (e.g. from "&&" or trailing "&")
        if (!segment.empty()) {
            auto const eq_pos = segment.find('=');
            std::string_view key_view;
            std::string_view value_view;

            if (eq_pos == std::string_view::npos) {
                // No '=' – whole segment is the key, value is empty
                key_view = segment;
            } else {
                key_view = segment.substr(0, eq_pos);
                value_view = segment.substr(eq_pos + 1);
            }

            auto decode_query = [plus_to_space](std::string_view s) -> std::string {
                // '+' -> space must happen on the raw segment, before
                // percent-decoding — otherwise "%2B" (an encoded literal
                // '+') would be wrongly turned into a space.
                std::string raw{s};
                if (plus_to_space) {
                    std::replace(raw.begin(), raw.end(), '+', ' ');
                }
                return Uri::url_decode(raw);
            };

            params.emplace_back(decode_query(key_view), decode_query(value_view));
        }

        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }

    return params;
}

// ---------------------------------------------------------------------------
// Static utilities
// ---------------------------------------------------------------------------

std::string Uri::url_encode(std::string_view input, bool encode_slash) {
    return yaddnsc::util::url_encode(input, encode_slash);
}

std::string Uri::url_decode(std::string_view input) {
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
