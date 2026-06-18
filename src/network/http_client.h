//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_NETWORK_HTTPCLIENT_H
#define YADDNSC_NETWORK_HTTPCLIENT_H

#include <map>
#include <string>
#include <string_view>

#include "type.h"

struct http_request;

namespace httplib {
    class Result;

    class Client;
}

class Uri;

namespace HttpClient {
    using param_type = std::multimap<std::string, std::string>;

    namespace detail {
        httplib::Client connect(const Uri &uri, address_family family, std::string_view nif_name = {});
        std::string build_request(const Uri &);
    }

    httplib::Result get(const Uri &, address_family, std::string_view nif_name = {});

    httplib::Result post(const Uri &, const param_type &, address_family, std::string_view nif_name = {});

    httplib::Result patch(const Uri &, const param_type &, address_family, std::string_view nif_name = {});

    httplib::Result put(const Uri &, const param_type &, address_family, std::string_view nif_name = {});

    httplib::Result del(const Uri &, const param_type &, address_family, std::string_view nif_name = {});

    // General-purpose request: accepts an http_request (any method, any body type, custom headers)
    httplib::Result send(const http_request &, address_family, std::string_view nif_name = {});
}

#endif //YADDNSC_NETWORK_HTTPCLIENT_H
