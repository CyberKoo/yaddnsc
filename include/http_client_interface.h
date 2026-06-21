//
// Created by Kotarou on 2026/6/20.
//

#ifndef YADDNSC_HTTP_CLIENT_INTERFACE_H
#define YADDNSC_HTTP_CLIENT_INTERFACE_H

#include <map>
#include <string>

#include "type.h"
#include "http_types.h"

struct HttpResponse final {
    bool success{false};
    int status_code{0};
    std::multimap<std::string, std::string> headers;
    std::string body;
    std::string error_message;
};

class IHttpSender {
public:
    virtual ~IHttpSender() = default;

    virtual void set_address_family(address_family af) = 0;

    virtual HttpResponse send(const http_request &req) = 0;
};

#endif //YADDNSC_HTTP_CLIENT_INTERFACE_H
