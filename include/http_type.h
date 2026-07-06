//
// Created by Kotarou on 2026/6/18.
//

#ifndef YADDNSC_HTTP_TYPE_H
#define YADDNSC_HTTP_TYPE_H

#include <map>
#include <string>
#include <expected>
#include <optional>

/// Key-value multimap for HTTP headers and query parameters.
using HttpParams = std::multimap<std::string, std::string>;

/// HTTP request methods supported by the client.
enum class HttpMethod {
    GET,     ///< HTTP GET
    POST,    ///< HTTP POST
    PUT,     ///< HTTP PUT
    DEL,     ///< HTTP DELETE
    PATCH,   ///< HTTP PATCH
    HEAD,    ///< HTTP HEAD
    OPTIONS  ///< HTTP OPTIONS
};

/// An outgoing HTTP request.
struct HttpRequest {
    std::string content_type;            ///< MIME type (e.g. "application/json")
    HttpMethod method;                   ///< Request method
    HttpParams headers{};                ///< Request headers
    std::optional<std::string> body{};   ///< Request body (absent for GET/HEAD)
};

/// An incoming HTTP response — status code, body and headers.
struct HttpResponse {
    int status_code;                                    ///< HTTP status code (e.g. 200, 404)
    std::string body;                                   ///< Response body
    std::multimap<std::string, std::string> headers;    ///< Response headers
};

/// Result type for HTTP operations: HttpResponse on success, error string on failure.
using HttpResult = std::expected<HttpResponse, std::string>;

#endif //YADDNSC_HTTP_TYPE_H
