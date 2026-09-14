//
// PersistentClient — the net::http HTTP client with connection reuse.
//
// Bound to one origin (base URL) at construction; exchange() takes a path
// and reuses a single connection across calls. Use Client for transient
// one-URL-per-request traffic.
//

#ifndef YADDNSC_HTTP_CLIENT_PERSISTENT_CLIENT_H
#define YADDNSC_HTTP_CLIENT_PERSISTENT_CLIENT_H

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

#include "interface/http_client.h"

#include "http_client/error.h"
#include "http_client/session.h"
#include "http_client/stream_factory.h"
#include "http_client/types.h"
#include "uri.h"

namespace Utils {
class CancellationToken;
}

namespace net::http {

/// Persistent HTTP client: fixed origin, keep-alive connection.
///
/// Also implements the HttpClient port: through the port, the `url`
/// parameter of exchange() carries the same path semantics (the origin
/// always comes from the base URL).
class PersistentClient final : public HttpClient {
public:
    /// @param base_url  Origin, e.g. "https://api.example.com" (path/query
    ///                  are allowed and used only for relative fallback).
    /// @throws std::invalid_argument when base_url is not a valid http(s) URL.
    explicit PersistentClient(std::string base_url, Options opts = {});

    /// Base URL + cancellation token bound into the connection.
    PersistentClient(std::string base_url, Options opts, Utils::CancellationToken token);

    /// Base URL + custom stream factory (tests inject fakes).
    PersistentClient(std::string base_url, Options opts, std::shared_ptr<StreamFactory> factory);

    ~PersistentClient() override;

    /// Perform an exchange over the persistent connection.
    /// @param url  Request target (path + query), e.g. "/v1/update?foo=bar".
    ///             Empty means "/".
    [[nodiscard]] std::expected<Response, Error> exchange(std::string_view url,
                                                          const Request& req) const override;

private:
    [[nodiscard]] Uri current_uri(const std::string_view target) const;

    std::string scheme_;
    std::string host_;
    std::uint16_t port_;
    Uri base_uri_;
    Options opts_;
    std::shared_ptr<StreamFactory> factory_;
    mutable Session session_;
};

} // namespace net::http

#endif // YADDNSC_HTTP_CLIENT_PERSISTENT_CLIENT_H
