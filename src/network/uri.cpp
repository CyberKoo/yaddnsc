//
// Created by Kotarou on 2021/9/7.
//
#include "uri.h"
#include "string_util.hpp"

#include <string>
#include <vector>
#include <utility>
#include <unordered_map>
#include <charconv>
#include <optional>
#include <algorithm>
#include <string_view>
#include <cctype>
#include <cstddef>

#include "fmt.hpp"
#include "network/inet_address.h"

namespace {
    /// Known scheme-to-default-port mappings.
    const std::unordered_map<std::string_view, int> KNOWN_PORTS = {
        {"http", 80}, {"https", 443}, {"tls", 853},
    };

    constexpr std::string_view DEFAULT_PATH = "/";

    constexpr auto HEX_CHARS = std::to_array("0123456789ABCDEF");

    [[nodiscard]] int lookup_default_port(std::string_view scheme) noexcept {
        auto it = KNOWN_PORTS.find(scheme);
        return it != KNOWN_PORTS.end() ? it->second : 0;
    }

    [[nodiscard]] bool is_default_port(std::string_view scheme, int port) noexcept {
        auto it = KNOWN_PORTS.find(scheme);
        return it != KNOWN_PORTS.end() && it->second == port;
    }

    /// Lowercase a range of characters in-place within a string.
    void lowercase_range(std::string &s, std::size_t pos, std::size_t len) noexcept {
        if (len == 0) return;
        auto start = s.begin() + static_cast<std::ptrdiff_t>(pos);
        std::transform(start, start + static_cast<std::ptrdiff_t>(len), start,
                       [](unsigned char c) -> char {
                           return static_cast<char>(std::tolower(c));
                       }
        );
    }
}

// ---------------------------------------------------------------------------
// Parse
// ---------------------------------------------------------------------------

Uri Uri::parse(std::string_view uri) {
    Uri result{};

    if (uri.empty()) {
        return result;
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
    std::size_t authority_start = 0; // offset where authority begins

    auto const hier_delim = u.find("://");
    if (hier_delim != std::string_view::npos && hier_delim > 0) {
        has_scheme = true;

        result.schema_.assign(0, hier_delim);
        // scheme is case-insensitive → lowercase in-place within raw_uri_
        lowercase_range(result.raw_uri_, 0, hier_delim);

        authority_start = hier_delim + 3; // skip "://"
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
        result.path_.assign(0, u.size());
        return result;
    }

    // -------- extract authority -------------------------------------------
    // Authority runs from authority_start up to the earliest structural
    // delimiter (path, query, or end-of-string).
    auto authority_end = u.size();
    if (path_pos != std::string_view::npos) {
        authority_end = (std::min)(authority_end, path_pos);
    }
    if (query_pos != std::string_view::npos) {
        authority_end = (std::min)(authority_end, query_pos);
    }

    auto const auth_view = u.substr(authority_start, authority_end - authority_start);
    parse_authority(auth_view, result.host_, result.port_, result.is_ipv6_, authority_start, result.raw_uri_);

    // host is case-insensitive per RFC 3986 §3.2.2 → lowercase in-place
    if (!result.host_.empty()) {
        lowercase_range(result.raw_uri_, result.host_.pos, result.host_.len);
    }

    // -------- default port ------------------------------------------------
    // Only apply well-known defaults when a scheme was explicitly given.
    // Bare host:port inputs keep whatever port was (or was not) specified.
    if (!result.port_.has_value()) {
        int const fallback = has_scheme ? default_port_for(result.schema_.view(result.raw_uri_)) : 0;
        result.port_.emplace(fallback);
    }

    // -------- path --------------------------------------------------------
    if (path_pos != std::string_view::npos &&
        (query_pos == std::string_view::npos || path_pos < query_pos)) {
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

void Uri::parse_authority(std::string_view auth, Slice &host_out, std::optional<int> &port_out, bool &is_ipv6_out,
                          std::size_t auth_raw_offset, std::string_view raw_uri_hint) {
    if (auth.empty()) {
        return;
    }

    if (auth.starts_with('[')) {
        auto const closing = auth.find(']');
        if (closing == std::string_view::npos) {
            throw std::runtime_error(
                fmt::format("Unclosed IPv6 literal bracket: {}", raw_uri_hint));
        }

        host_out.assign(auth_raw_offset + 1, closing - 1);
        is_ipv6_out = true;

        if (closing + 1 < auth.size() && auth[closing + 1] == ':') {
            auto const port_str = auth.substr(closing + 2);
            if (!port_str.empty()) {
                int v{};
                if (auto [p, ec] = std::from_chars(
                        port_str.data(), port_str.data() + port_str.size(), v);
                    ec == std::errc()) {
                    port_out.emplace(v);
                }
            }
        }
        return;
    }

    // Bare IPv6 (e.g. ::1, 2001:db8::1)
    // Only attempt Inet6Address::parse when auth contains ':' — plain
    // hostnames and IPv4 addresses never have one, so this avoids an
    // expensive inet_pton call on every parse.
    if (auth.contains(':') && Inet6Address::parse(auth)) {
        host_out.assign(auth_raw_offset, auth.size());
        is_ipv6_out = true;
        return;
    }

    // Possibly host:port  (exactly one colon)
    auto const colon = auth.rfind(':');
    if (colon != std::string_view::npos) {
        host_out.assign(auth_raw_offset, colon);
        auto const port_str = auth.substr(colon + 1);
        if (!port_str.empty()) {
            int v{};
            if (auto [p, ec] = std::from_chars(port_str.data(), port_str.data() + port_str.size(), v);
                ec == std::errc()) {
                port_out.emplace(v);
            }
        }
        return;
    }

    host_out.assign(auth_raw_offset, auth.size());
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
    // port_ is always populated after parse() — either explicitly set or
    // filled in by the default-port logic — so value() is safe here.
    return *port_;
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

    if (schema_view.empty()) {
        if (*port_ != 0) {
            return std::string(host_view) + ':' + std::to_string(*port_);
        }
        return std::string(host_view);
    }
    if (is_default_port(schema_view, *port_)) {
        return std::string(schema_view) + "://" + std::string(host_view);
    }
    return std::string(schema_view) + "://" + std::string(host_view) + ':' + std::to_string(*port_);
}

std::vector<std::pair<std::string, std::string> > Uri::get_query_params(bool plus_to_space) const {
    std::vector<std::pair<std::string, std::string> > params;

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
                auto decoded = Uri::url_decode(s);
                if (plus_to_space) {
                    for (auto &ch: decoded) {
                        if (ch == '+') ch = ' ';
                    }
                }
                return decoded;
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

std::string Uri::url_encode(std::string_view input) noexcept {
    std::string result;
    result.reserve(input.size() * 3);

    for (auto const c: input) {
        auto const uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || uc == '-' || uc == '.' || uc == '_' || uc == '~') {
            // unreserved character (RFC 3986 §2.3)
            result += c;
        } else {
            result += '%';
            result += HEX_CHARS[uc >> 4];
            result += HEX_CHARS[uc & 0xF];
        }
    }

    return result;
}

std::string Uri::url_decode(std::string_view input) noexcept {
    std::string result;
    result.reserve(input.size());

    for (std::size_t i = 0; i < input.size(); ++i) {
        if (auto const c = input[i]; c == '%' && i + 2 < input.size()) {
            auto const hex_pair = std::array{input[i + 1], input[i + 2]};
            unsigned int val{};
            if (auto [p, ec] = std::from_chars(hex_pair.data(), hex_pair.data() + hex_pair.size(), val, 16);
                ec == std::errc{}) {
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
