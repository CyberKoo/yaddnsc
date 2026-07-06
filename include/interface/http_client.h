//
// Created by Kotarou on 2026/6/20.
//

#ifndef YADDNSC_HTTP_CLIENT_INTERFACE_H
#define YADDNSC_HTTP_CLIENT_INTERFACE_H

#include <optional>
#include <string>

#include "mixin.h"
#include "http_type.h"
#include "yaddnsc_export.h"

class YADDNSC_EXPORT HttpClient {
public:
    virtual ~HttpClient() = default;

    [[nodiscard]] virtual HttpResult exchange(std::string_view url, const HttpRequest &req) const = 0;

    // One-shot GET — returns the raw response body on success.
    [[nodiscard]] std::optional<std::string> get_body(std::string_view url) const;

    // Form-encode a parameter map into application/x-www-form-urlencoded string.
    static std::string params_to_query_string(const HttpParams &params);

private:
    [[maybe_unused, no_unique_address]] NoCopy _nc_;
    [[maybe_unused, no_unique_address]] NoMove _nm_;
};

#endif //YADDNSC_HTTP_CLIENT_INTERFACE_H
