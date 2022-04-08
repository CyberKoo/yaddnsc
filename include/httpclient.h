//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_HTTPCLIENT_H
#define YADDNSC_HTTPCLIENT_H

#include <map>
#include <string_view>
#include <sys/socket.h>

#include "common_fwd.h"

class HttpClient {
public:
    using Params = std::multimap<std::string, std::string>;

    static std::unique_ptr<httplib::Client> connect(const Uri &, int, const char *);

    static httplib::Result get(const Uri &, int = AF_UNSPEC, const char * = nullptr);

    static httplib::Result post(const Uri &, const Params &, int = AF_UNSPEC, const char * = nullptr);

    static httplib::Result put(const Uri &, const Params &, int = AF_UNSPEC, const char * = nullptr);

    static std::string build_request(const Uri &);

private:
    static std::string_view get_system_ca_path();
};

#endif //YADDNSC_HTTPCLIENT_H
