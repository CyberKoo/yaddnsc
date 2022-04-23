//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_HTTPCLIENT_H
#define YADDNSC_HTTPCLIENT_H

#include <map>
#include <string_view>

namespace httplib {
    class Result;

    class Client;
};

class Uri;

namespace HttpClient {
    using param_t = std::multimap<std::string, std::string>;

    httplib::Client connect(const Uri &uri, int family, const char *nif_name);

    httplib::Result get(const Uri &, int, const char * = nullptr);

    httplib::Result post(const Uri &, const param_t &, int, const char * = nullptr);

    httplib::Result put(const Uri &, const param_t &, int, const char * = nullptr);

    std::string build_request(const Uri &);
};

#endif //YADDNSC_HTTPCLIENT_H
