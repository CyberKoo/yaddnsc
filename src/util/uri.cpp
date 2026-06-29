//
// Created by Kotarou on 2021/9/7.
//
#include "uri.h"

#include <string>
#include <unordered_map>
#include <charconv>
#include <optional>
#include <algorithm>
#include <string_view>

#include "fmt.hpp"
#include "network/inet_address.h"

namespace {
    /// Known scheme-to-default-port mappings.
    const std::unordered_map<std::string_view, int> known_ports = {
        {"http", 80}, {"https", 443}, {"tls", 853},
    };

    int lookup_default_port(std::string_view scheme) noexcept {
        auto it = known_ports.find(scheme);
        return it != known_ports.end() ? it->second : 0;
    }

    bool is_default_port(std::string_view scheme, int port) noexcept {
        auto it = known_ports.find(scheme);
        return it != known_ports.end() && it->second == port;
    }

    /// Parse a host:port authority string into host and port.
    ///
    /// Expects a view that contains only the authority portion (no path/query).
    /// Handles IPv6 literals (`[::1]` / `[::1]:53`), bare IPv6 (`::1`, `2001:db8::1`), host:port, and plain hostname/IPv4.
    void parse_authority(std::string_view auth, std::string &host_out, std::optional<int> &port_out,
                         std::string_view raw_uri_hint) {
        if (auth.empty()) {
            return;
        }

        if (auth.starts_with('[')) {
            auto const closing = auth.find(']');
            if (closing == std::string_view::npos) {
                throw std::runtime_error(
                    fmt::format("Unclosed IPv6 literal bracket: {}", raw_uri_hint));
            }

            host_out.assign(auth.substr(1, closing - 1));

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
        if (Inet6Address::parse(auth)) {
            host_out.assign(auth);
            return;
        }

        // Possibly host:port  (exactly one colon)
        auto const colon = auth.rfind(':');
        if (colon != std::string_view::npos) {
            host_out.assign(auth.substr(0, colon));
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

        host_out.assign(auth);
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

    result.raw_uri_ = uri;

    // -------- strip fragment (#...) ---------------------------------------
    // Fragment is always the very last component per RFC 3986.
    if (auto const frag_pos = uri.find('#'); frag_pos != std::string_view::npos) {
        uri.remove_suffix(uri.size() - frag_pos);
    }

    // -------- scheme detection --------------------------------------------
    // Look for "://".  If found and there is at least one character before
    // it, treat the part before "://" as a scheme name.
    bool has_scheme = false;
    std::size_t authority_start = 0; // offset where authority begins

    auto const hier_delim = uri.find("://");
    if (hier_delim != std::string_view::npos && hier_delim > 0) {
        has_scheme = true;

        result.schema_ = std::string(uri.substr(0, hier_delim));
        // scheme is case-insensitive → normalize to lowercase
        std::ranges::transform(
            result.schema_, result.schema_.begin(),
            [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            }
        );

        authority_start = hier_delim + 3; // skip "://"
        result.body_ = std::string(uri.substr(authority_start));
    } else {
        // No scheme – the whole input (minus fragment) is treated as
        // authority, path, or a combination thereof.
        result.body_ = std::string(uri);
    }

    // -------- locate path & query boundaries ------------------------------
    auto const query_pos = uri.find('?', authority_start);
    auto const path_pos = uri.find('/', authority_start);

    // -------- path-only inputs (no authority) -----------------------------
    // Relative/absolute paths with no scheme and no host.
    if (!has_scheme && (uri.starts_with('/') || uri.starts_with('.'))) {
        result.path_ = std::string(uri);
        return result;
    }

    // -------- extract authority -------------------------------------------
    // Authority runs from authority_start up to the earliest structural
    // delimiter (path, query, or end-of-string).
    auto authority_end = uri.size();
    if (path_pos != std::string_view::npos) {
        authority_end = (std::min)(authority_end, path_pos);
    }
    if (query_pos != std::string_view::npos) {
        authority_end = (std::min)(authority_end, query_pos);
    }

    parse_authority(uri.substr(authority_start, authority_end - authority_start),
                    result.host_, result.port_, result.raw_uri_);

    // host is case-insensitive per RFC 3986 §3.2.2
    std::ranges::transform(
        result.host_, result.host_.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        }
    );

    // Pre-compute the bracketed form so get_host_bracketed() is O(1).
    if (Inet6Address::parse(result.host_)) {
        result.host_bracketed_ = std::string{'['} + result.host_ + ']';
    } else {
        result.host_bracketed_ = result.host_;
    }

    // -------- default port ------------------------------------------------
    // Only apply well-known defaults when a scheme was explicitly given.
    // Bare host:port inputs keep whatever port was (or was not) specified.
    if (!result.port_.has_value()) {
        int const fallback = has_scheme ? default_port_for(result.schema_) : 0;
        result.port_.emplace(fallback);
    }

    // -------- path --------------------------------------------------------
    if (path_pos != std::string_view::npos) {
        // Path runs from the first '/' up to (but not including) '?'.
        // If a pathological '?' appears before '/', ignore it.
        auto const path_end = (query_pos != std::string_view::npos && query_pos > path_pos)
                                  ? query_pos
                                  : uri.size();
        result.path_ = std::string(uri.substr(path_pos, path_end - path_pos));
    }

    // -------- query string ------------------------------------------------
    if (query_pos != std::string_view::npos) {
        result.query_string_ = std::string(uri.substr(query_pos + 1));
    }

    // -------- default path ------------------------------------------------
    // An empty path with an explicit authority (scheme + host) defaults to "/"
    // per RFC 3986 §3.3.
    if (result.path_.empty() && has_scheme) {
        result.path_ = "/";
    }

    return result;
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

std::string_view Uri::get_query_string() const noexcept { return query_string_; }
std::string_view Uri::get_path() const noexcept { return path_; }
std::string_view Uri::get_schema() const noexcept { return schema_; }
std::string_view Uri::get_host() const noexcept { return host_; }

std::string_view Uri::get_host_literal() const noexcept {
    return host_bracketed_;
}

std::string_view Uri::get_raw_uri() const noexcept { return raw_uri_; }

std::string Uri::get_origin() const {
    if (schema_.empty()) {
        if (*port_ != 0) {
            return host_bracketed_ + ":" + std::to_string(*port_);
        }
        return host_bracketed_;
    }
    if (is_default_port(schema_, *port_)) {
        return schema_ + "://" + host_bracketed_;
    }
    return schema_ + "://" + host_bracketed_ + ":" + std::to_string(*port_);
}

std::string_view Uri::get_body() const noexcept { return body_; }

int Uri::get_port() const noexcept {
    // port_ is always populated after parse() — either explicitly set or
    // filled in by the default-port logic above — so value() is safe here.
    return *port_;
}
