//
// Created by Kotarou on 2026/6/18.
//

#ifndef YADDNSC_HTTP_TYPE_H
#define YADDNSC_HTTP_TYPE_H

#include <map>
#include <string>
#include <expected>
#include <optional>

using HttpParams = std::multimap<std::string, std::string>;

enum class HttpMethod {
    GET, POST, PUT, DEL, PATCH, HEAD, OPTIONS
};

struct HttpRequest {
    std::string content_type;
    HttpMethod method;
    HttpParams headers{};
    std::optional<std::string> body{};
};

// HTTP response — status code, body and headers from a received response.
struct HttpResponse {
    int status_code;
    std::string body;
    std::multimap<std::string, std::string> headers;
};

using HttpResult = std::expected<HttpResponse, std::string>;

#endif //YADDNSC_HTTP_TYPE_H
