//
// Client — the net::http HTTP client.
//
// Cancellation, connection lifecycle and TLS are entirely internal: the
// caller sees one exchange(url, req) operation. Each exchange uses a fresh
// connection (transient mode); use Session for persistent reuse.
//
// Client is the production implementation of the HttpClient port.
//

#ifndef YADDNSC_HTTP_CLIENT_CLIENT_H
#define YADDNSC_HTTP_CLIENT_CLIENT_H

#include <memory>
#include <string_view>

#include "infrastructure/network/http/client_port.h"
#include "infrastructure/network/http/types.h"

namespace net {
namespace http {
class StreamFactory;
}  // namespace http
}  // namespace net

namespace Utils {
class CancellationToken;
}

namespace net::http {

class Client final : public HttpClient {
public:
    /// Construct with the default stream factory and no cancellation.
    explicit Client(Options opts);

    /// Construct with the default stream factory and a cancellation token
    /// bound into every connection the client makes.
    Client(Options opts, Utils::CancellationToken token);

    /// Construct with a custom stream factory (tests inject fakes).
    Client(Options opts, std::shared_ptr<StreamFactory> factory);

    /// Perform an HTTP exchange, following redirects per Options.
    [[nodiscard]] std::expected<Response, Error> exchange(std::string_view url, const Request& req) const override;

private:
    Options opts_;
    std::shared_ptr<StreamFactory> factory_;
};

}  // namespace net::http

#endif  // YADDNSC_HTTP_CLIENT_CLIENT_H
