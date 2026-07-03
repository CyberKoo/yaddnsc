//
// Created by Kotarou on 2026/6/20.
//

#ifndef YADDNSC_HTTP_CLIENT_INTERFACE_H
#define YADDNSC_HTTP_CLIENT_INTERFACE_H

#include <expected>
#include <map>
#include <string>

#include "mixin.h"
#include "http_type.h"
#include "yaddnsc_export.h"

// HTTP response — status code, body and headers from a received response.
// Does NOT imply business success; check status_code separately.
struct HttpResponse {
    int status_code;
    std::string body;
    std::multimap<std::string, std::string> headers;
};

using HttpResult = std::expected<HttpResponse, std::string>;

class YADDNSC_EXPORT HttpClient {
public:
    virtual ~HttpClient() = default;

    [[nodiscard]] virtual HttpResult send(const HttpRequest &req) const = 0;

    // Form-encode a parameter map into application/x-www-form-urlencoded string.
    static std::string params_to_query_string(const HttpParams &params);

private:
    [[maybe_unused, no_unique_address]] NoCopy _nc_;
    [[maybe_unused, no_unique_address]] NoMove _nm_;
};

#endif //YADDNSC_HTTP_CLIENT_INTERFACE_H
