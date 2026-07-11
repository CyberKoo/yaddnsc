//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_URI_H
#define YADDNSC_URI_H

#include <string>
#include <vector>
#include <utility>
#include <optional>
#include <string_view>

/// A URI parser and builder conforming to RFC 3986.
///
/// Parses a URI string into its components (scheme, host, port, path,
/// query string) and provides helpers for percent-encoding/decoding,
/// origin computation, and query-string parameter extraction.
class Uri {
public:
    /// Parse a URI from its string representation.
    /// @param uri  The URI string to parse (e.g. "https://example.com:8080/path?q=1#frag").
    ///             The fragment component, if present, is ignored.
    /// @return     A fully populated Uri instance.
    static Uri parse(std::string_view uri);

    /// Return the scheme component.
    /// e.g. for "https://example.com/path" -> "https"
    /// @return The URI scheme, or empty string if not present.
    [[nodiscard]] std::string_view get_schema() const noexcept;

    /// Return the host component (without brackets for IPv6).
    /// e.g. for "https://example.com/path" -> "example.com"
    ///      for "https://[::1]:8080/path"  -> "::1"
    /// @return The host string, or empty string if not present.
    [[nodiscard]] std::string_view get_host() const noexcept;

    /// Return the host as an RFC 3986 IP-literal.
    /// IPv6 addresses are wrapped in `[]`; IPv4 and hostnames are unchanged.
    /// e.g. for "https://[::1]:8080/path" -> "[::1]"
    ///      for "https://example.com/path" -> "example.com"
    /// @return The host in RFC 3986 IP-literal form.
    [[nodiscard]] std::string_view get_host_literal() const noexcept;

    /// Return the port number.
    /// @return The port if explicitly set in the URI, or 0 if absent
    ///         (including when no well-known default exists for the scheme).
    [[nodiscard]] int get_port() const noexcept;

    /// Return the path component.
    /// e.g. for "https://example.com/api/v1" -> "/api/v1"
    /// @return The URI path, or empty string if not present.
    [[nodiscard]] std::string_view get_path() const noexcept;

    /// Return the query string (without the leading '?').
    /// e.g. for "https://example.com/path?key=val" -> "key=val"
    /// @return The query string, or empty string if not present.
    [[nodiscard]] std::string_view get_query_string() const noexcept;

    /// Parse the query string and return decoded key-value pairs.
    /// Empty query string yields an empty vector.
    /// Empty segments (e.g. from "&&") are silently skipped.
    /// Malformed percent-encoding (e.g. "%GG", trailing "%") is preserved as-is.
    ///
    /// @param plus_to_space  When true, '+' is decoded as space
    ///                       (application/x-www-form-urlencoded convention).
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> get_query_params(bool plus_to_space = true) const;

    /// Return the body component (non-standard; populated from custom parsing rules).
    /// @return The URI body component, or empty string if not present.
    [[nodiscard]] std::string_view get_body() const noexcept;

    /// Return the origin (schema://host_literal), omitting the port when it matches
    /// the well-known default for the scheme (e.g. 443 for https).
    /// @return The origin string (e.g. "https://example.com").
    [[nodiscard]] std::string get_origin() const;

    /// Return the original raw URI string as passed to parse().
    /// @return The raw URI string passed to Uri::parse().
    [[nodiscard]] std::string_view get_raw_uri() const noexcept;

    /// Percent-encode a string per RFC 3986 §2.1.
    /// Unreserved characters (A-Z, a-z, 0-9, '-', '.', '_', '~') are passed through;
    /// all other bytes are encoded as "%XX" (uppercase hex).
    [[nodiscard]] static std::string url_encode(std::string_view input) noexcept;

    /// Percent-decode a string per RFC 3986 §2.1.
    /// Each "%XX" sequence is replaced with the corresponding byte.
    /// Malformed sequences (e.g. "%GG", trailing "%") are preserved as-is.
    /// Note: '+' is NOT decoded as space; that convention belongs to
    /// application/x-www-form-urlencoded and is handled by get_query_params().
    [[nodiscard]] static std::string url_decode(std::string_view input) noexcept;

private:
    Uri() = default;

    /// Return the well-known default port for a given scheme, or 0 if unknown.
    static int default_port_for(std::string_view scheme) noexcept;

    std::string schema_;
    std::string host_;
    std::string host_bracketed_;
    std::optional<int> port_;       // std::nullopt = not explicitly set in URI
    std::string path_;
    std::string query_string_;
    std::string body_;
    std::string raw_uri_;
};

#endif //YADDNSC_URI_H
