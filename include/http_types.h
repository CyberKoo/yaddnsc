//
// Created by Kotarou on 2026/6/18.
//

#ifndef YADDNSC_HTTP_TYPES_H
#define YADDNSC_HTTP_TYPES_H

#include <map>
#include <string>
#include <variant>

using http_param_type = std::multimap<std::string, std::string>;

enum class http_method_type {
    GET, POST, PUT, DEL, PATCH
};

struct http_request {
    std::string url;
    std::variant<http_param_type, std::string> body;
    std::string content_type;
    http_method_type request_method;
    http_param_type header;
};

#endif //YADDNSC_HTTP_TYPES_H
