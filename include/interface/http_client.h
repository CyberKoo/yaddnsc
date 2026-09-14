//
// Created by Kotarou on 2026/6/20.
//

#ifndef YADDNSC_HTTP_CLIENT_INTERFACE_H
#define YADDNSC_HTTP_CLIENT_INTERFACE_H

#include <expected>
#include <string>
#include <string_view>

#include "http_client/error.h"
#include "http_client/types.h"
#include "mixin.h"
#include "yaddnsc_export.h"

/// Abstract HTTP client interface.
///
/// Implementations (e.g. net::http::Client, MockHttpClient) provide the
/// actual transport. The interface is designed to be mock-friendly for
/// unit testing. Errors are structured net::http::Error values.
class YADDNSC_EXPORT HttpClient {
public:
    virtual ~HttpClient() = default;

    /// Perform an HTTP exchange and return the response or an error.
    /// @param url   Target URL.
    /// @param req   Request details (method, headers, body, content type).
    /// @return      Response on success, or a structured error on failure.
    [[nodiscard]] virtual std::expected<net::http::Response, net::http::Error>
        exchange(std::string_view url, const net::http::Request &req) const = 0;

private:
    [[maybe_unused, no_unique_address]] NoCopy no_copy_;
    [[maybe_unused, no_unique_address]] NoMove no_move_;
};

#endif //YADDNSC_HTTP_CLIENT_INTERFACE_H
