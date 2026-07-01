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

class Uri {
public:
    static Uri parse(std::string_view uri);

    [[nodiscard]] std::string_view get_query_string() const noexcept;

    [[nodiscard]] std::string_view get_path() const noexcept;

    [[nodiscard]] std::string_view get_schema() const noexcept;

    [[nodiscard]] std::string_view get_host() const noexcept;

    /// Return the host as an RFC 3986 IP-literal.
    /// IPv6 addresses are wrapped in `[]`; IPv4 and hostnames are unchanged.
    [[nodiscard]] std::string_view get_host_literal() const noexcept;

    [[nodiscard]] int get_port() const noexcept;

    [[nodiscard]] std::string_view get_body() const noexcept;

    [[nodiscard]] std::string_view get_raw_uri() const noexcept;

    /// Return the origin (schema://host_literal), omitting the port when it matches
    /// the well-known default for the scheme (e.g. 443 for https).
    [[nodiscard]] std::string get_origin() const;

    /// Parse the query string and return decoded key-value pairs.
    /// Empty query string yields an empty vector.
    /// Empty segments (e.g. from "&&") are silently skipped.
    /// Malformed percent-encoding (e.g. "%GG", trailing "%") is preserved as-is.
    ///
    /// @param plus_to_space  When true, '+' is decoded as space
    ///                       (application/x-www-form-urlencoded convention).
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> get_query_params(bool plus_to_space = true) const;

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

    static int default_port_for(std::string_view scheme) noexcept;

    std::string query_string_;
    std::string path_;
    std::string schema_;
    std::string host_;
    std::string host_bracketed_;
    std::string raw_uri_;
    std::string body_;
    std::optional<int> port_;     // nullopt = not explicitly set
};

#endif //YADDNSC_URI_H
