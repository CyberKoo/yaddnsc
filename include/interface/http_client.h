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

/// Abstract HTTP client interface.
///
/// Implementations (e.g. TransientHttpClient, MockHttpClient) provide the
/// actual transport layer.  The interface is designed to be mock-friendly
/// for unit testing.
class YADDNSC_EXPORT HttpClient {
public:
    virtual ~HttpClient() = default;

    /// Perform an HTTP exchange and return the response or an error message.
    /// @param url   Target URL.  May be empty when an implementation (e.g.
    ///              PersistentHttpClient) already knows the target and
    ///              falls back to its construction-time URI.
    /// @param req   Request details (method, headers, body, content type).
    /// @return      HttpResponse on success, or an error string on failure.
    [[nodiscard]] virtual HttpResult exchange(std::string_view url, const HttpRequest &req) const = 0;

    /// One-shot GET — returns the raw response body on success.
    /// @param url  Target URL.
    /// @return     The response body, or std::nullopt on failure.
    [[nodiscard]] std::optional<std::string> get_body(std::string_view url) const;

    /// Form-encode a parameter map into application/x-www-form-urlencoded string.
    /// @param params  Key-value pairs to encode.
    /// @return        URL-encoded query string (e.g. "key1=val1&key2=val2").
    [[nodiscard]] static std::string params_to_query_string(const HttpParams &params);

private:
    [[maybe_unused, no_unique_address]] NoCopy no_copy_;
    [[maybe_unused, no_unique_address]] NoMove no_move_;
};

#endif //YADDNSC_HTTP_CLIENT_INTERFACE_H
