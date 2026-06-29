//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_URI_H
#define YADDNSC_URI_H

#include <string>
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

private:
    Uri() = default;

    static int default_port_for(std::string_view scheme) noexcept;

private:
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
