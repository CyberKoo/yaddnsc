//
// Created by Kotarou on 2021/9/7.
//
#include "uri.h"

#include <string>
#include <algorithm>
#include <charconv>
#include <string_view>

#include "fmt.hpp"

Uri Uri::parse(std::string_view uri) {
    Uri result;

    if (uri.empty()) {
        return result;
    }

    result.raw_uri_ = uri;

    // get query start
    auto query_start = std::ranges::find(uri, '?');

    // schema
    auto schema_end = std::ranges::find(uri, ':');

    if (schema_end != uri.end()) {
        auto remaining = std::string_view(schema_end, uri.end());
        if (remaining.starts_with("://")) {
            result.schema_ = std::string(uri.begin(), schema_end);

            std::ranges::transform(
                result.schema_, result.schema_.begin(),
                [](unsigned char c) { return std::tolower(c); }
            );

            schema_end += 3; // skip ://
        } else {
            schema_end = uri.begin(); // no schema
        }
    } else {
        schema_end = uri.begin(); // no schema
    }

    if (result.schema_.empty()) {
        throw std::runtime_error(fmt::format("URI does not have a valid schema: {}", uri));
    }

    result.body_ = std::string(schema_end, uri.end());

    // --- host & port (with IPv6 literal support) -------------------------

    auto authority_start = schema_end;

    // Find the start of the path (first '/' after authority)
    auto path_start = std::find(authority_start, uri.end(), '/');
    // If there's a query before any path, adjust
    auto authority_end = (path_start != uri.end()) ? path_start : query_start;

    // Check for IPv6 literal: [::1]
    if (authority_start != authority_end && *authority_start == '[') {
        auto closing_bracket = std::find(authority_start + 1, authority_end, ']');
        if (closing_bracket == authority_end) {
            throw std::runtime_error(
                fmt::format("URI contains unclosed IPv6 literal bracket: {}", uri));
        }

        // Host is between [ and ]
        result.host_ = std::string(authority_start + 1, closing_bracket);

        // Look for port after ]
        auto port_start = closing_bracket + 1;
        if (port_start < authority_end && *port_start == ':') {
            ++port_start;
            auto port_str = std::string_view(port_start, authority_end);
            if (!port_str.empty()) {
                auto [ptr, ec] = std::from_chars(
                    port_str.data(), port_str.data() + port_str.size(), result.port_);
                if (ec != std::errc()) {
                    result.port_ = 0;
                }
            }
        }
    } else {
        // Plain host (IPv4 or hostname) — use ':' to split host and port
        auto host_end = std::find(authority_start, authority_end, ':');
        result.host_ = std::string(authority_start, host_end);

        if (host_end != authority_end) {
            // skip ':'
            ++host_end;
            auto port_str = std::string_view(host_end, authority_end);
            if (!port_str.empty()) {
                auto [ptr, ec] = std::from_chars(
                    port_str.data(), port_str.data() + port_str.size(), result.port_);
                if (ec != std::errc()) {
                    result.port_ = 0;
                }
            }
        }
    }

    // default ports
    if (result.port_ == 0) {
        if (result.schema_ == "http") {
            result.port_ = 80;
        } else if (result.schema_ == "https") {
            result.port_ = 443;
        }
    }

    // path
    if (path_start != uri.end()) {
        result.path_ = std::string(path_start, query_start);
    }

    // query
    if (query_start != uri.end()) {
        result.query_string_ = std::string(query_start + 1, uri.end());
    }

    return result;
}

std::string_view Uri::get_query_string() const {
    return query_string_;
}

std::string_view Uri::get_path() const {
    return path_;
}

std::string_view Uri::get_schema() const {
    return schema_;
}

std::string_view Uri::get_host() const {
    return host_;
}

int Uri::get_port() const {
    return port_;
}

std::string_view Uri::get_raw_uri() const {
    return raw_uri_;
}

std::string_view Uri::get_body() const {
    return body_;
}
