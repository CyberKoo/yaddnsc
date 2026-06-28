//
// Created by Kotarou on 2026/6/20.
//

#ifndef YADDNSC_HTTP_CLIENT_INTERFACE_H
#define YADDNSC_HTTP_CLIENT_INTERFACE_H

#include <expected>
#include <map>
#include <string>

#include "type.h"
#include "mixin.h"
#include "http_types.h"
#include "yaddnsc_export.h"

// HTTP response data — status code, body and headers from a received
// response.  Does NOT imply business success; check status_code separately.
struct HttpResponseData final {
    int status_code;
    std::string body;
    std::multimap<std::string, std::string> headers;
};

using HttpResponse = std::expected<HttpResponseData, std::string>;

class YADDNSC_EXPORT HttpClient {
public:
    virtual ~HttpClient() = default;

    virtual HttpResponse send(const http_request &req) const = 0;

    // Form-encode a parameter map into application/x-www-form-urlencoded string.
    static std::string params_to_query_string(const http_param_type &params);

private:
    [[maybe_unused, no_unique_address]] NoCopy _nc_;
    [[maybe_unused, no_unique_address]] NoMove _nm_;
};

#endif //YADDNSC_HTTP_CLIENT_INTERFACE_H
