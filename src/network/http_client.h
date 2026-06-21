//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_NETWORK_HTTPCLIENT_H
#define YADDNSC_NETWORK_HTTPCLIENT_H

#include <optional>
#include <string>
#include <string_view>

#include "type.h"
#include "http_client_interface.h"

// ---------------------------------------------------------------------------
// HttpClient — concrete IHttpSender that wraps cpp-httplib.
//
// Construct with an address family and optional network interface; use
// send() to issue arbitrary requests.  get_text() is a static convenience
// for one-shot GET calls (e.g. external-IP discovery).
// ---------------------------------------------------------------------------
class HttpClient final : public IHttpSender {
public:
    explicit HttpClient(address_family af, std::string_view interface = {});

    ~HttpClient() override = default;

    HttpClient(const HttpClient &) = delete;

    HttpClient &operator=(const HttpClient &) = delete;

    void set_address_family(address_family af) override;

    HttpResponse send(const http_request &req) override;

    // One-shot GET — returns the raw response body on success.
    static std::optional<std::string>
    get_body(std::string_view url, address_family af, std::string_view interface = {});

private:
    address_family af_;
    std::string interface_;
};

#endif //YADDNSC_NETWORK_HTTPCLIENT_H
