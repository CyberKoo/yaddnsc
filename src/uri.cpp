//
// Created by Kotarou on 2021/9/7.
//
#include "uri.h"

#include <string>
#include <algorithm>
#include <charconv>

#include "fmt.h"

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
        throw std::runtime_error(fmt::format("URI doesn't have a valid schema, {}", uri));
    }

    result.body_ = std::string(schema_end, uri.end());

    // host
    auto host_start = schema_end;
    auto path_start = std::find(host_start, uri.end(), '/');
    auto host_end = std::find(schema_end, (path_start != uri.end()) ? path_start : query_start, ':');

    result.host_ = std::string(host_start, host_end);

    // port
    if (host_end != uri.end() && host_end[0] == ':') {
        ++host_end;
        auto port_end = (path_start != uri.end()) ? path_start : query_start;
        auto port_str = std::string_view(host_end, port_end);
        if (!port_str.empty()) {
            auto [ptr, ec] = std::from_chars(port_str.data(), port_str.data() + port_str.size(), result.port_);
            if (ec != std::errc()) {
                result.port_ = 0;
            }
        }
    }

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
