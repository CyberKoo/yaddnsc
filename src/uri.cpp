//
// Created by Kotarou on 2021/9/7.
//
#include "uri.h"

#include <string>
#include <algorithm>
#include <fmt/format.h>

Uri Uri::parse(std::string_view uri) {
    Uri result;

    if (uri.empty()) {
        return result;
    }

    result.raw_uri = uri;

    // get query start
    auto query_start = std::find(uri.begin(), uri.end(), '?');

    // schema
    auto schema_start = uri.begin();
    auto schema_end = std::find(schema_start, uri.end(), ':');

    if (schema_end != uri.end()) {
        std::string port = &*(schema_end);
        if ((port.length() > 3) && (port.substr(0, 3) == "://")) {
            result.schema = std::string(schema_start, schema_end);

            std::transform(result.schema.begin(), result.schema.end(), result.schema.begin(),
                           [](unsigned char c) { return std::tolower(c); });

            schema_end += 3;   //      ://
        } else {
            schema_end = uri.begin();  // no schema
        }
    } else {
        schema_end = uri.begin();  // no schema
    }

    if (result.schema.empty()) {
        throw std::runtime_error(fmt::format("URI doesn't have a valid schema, {}", uri));
    }

    result.body = std::string(schema_end, uri.end());

    // host
    auto host_start = schema_end;
    auto path_start = std::find(host_start, uri.end(), '/');
    auto host_end = std::find(schema_end, (path_start != uri.end()) ? path_start : query_start, ':');

    result.host = std::string(host_start, host_end);

    // port
    if ((host_end != uri.end()) && ((&*(host_end))[0] == ':')) {
        host_end++;
        auto portEnd = (path_start != uri.end()) ? path_start : query_start;
        auto port = std::string(host_end, portEnd);
        if (!port.empty()) {
            result.port = std::stoi(port);
        }
    }

    if (result.port == 0) {
        if (result.schema == "http") {
            result.port = 80;
        } else if (result.schema == "https") {
            result.port = 443;
        }
    }

    // path
    if (path_start != uri.end()) {
        result.path = std::string(path_start, query_start);
    }

    // query
    if (query_start != uri.end()) {
        result.query_string = std::string(query_start + 1, uri.end());
    }

    return result;
}

std::string_view Uri::get_query_string() const {
    return query_string;
}

std::string_view Uri::get_path() const {
    return path;
}

std::string_view Uri::get_schema() const {
    return schema;
}

std::string_view Uri::get_host() const {
    return host;
}

int Uri::get_port() const {
    return port;
}

std::string_view Uri::get_raw_uri() const {
    return raw_uri;
}

std::string_view Uri::get_body() const {
    return body;
}
