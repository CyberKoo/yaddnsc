//
// Created by Kotarou on 2026/6/18.
//

#ifndef YADDNSC_HTTP_TYPE_H
#define YADDNSC_HTTP_TYPE_H

#include <map>
#include <string>
#include <optional>

using HttpParams = std::multimap<std::string, std::string>;

enum class HttpMethod {
    GET, POST, PUT, DEL, PATCH, HEAD, OPTIONS
};

struct HttpRequest {
    std::string url;
    std::string content_type;
    HttpMethod method;
    HttpParams headers{};
    std::optional<std::string> body{};
};

#endif //YADDNSC_HTTP_TYPE_H
