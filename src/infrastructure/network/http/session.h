//
// Session — persistent, thread-safe HTTP exchange over one reused
// connection (net::http).
//
// Owns a single transport stream for one origin and serializes access
// with an internal mutex. On a mid-exchange connection loss the stream is
// rebuilt and idempotent requests are retried once.
//

#ifndef YADDNSC_HTTP_CLIENT_SESSION_H
#define YADDNSC_HTTP_CLIENT_SESSION_H

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <expected>

#include "infrastructure/network/http/error.h"
#include "infrastructure/network/http/protocol/exchange.h"
#include "infrastructure/network/http/types.h"
#include "infrastructure/network/transport/options.h"
#include "infrastructure/network/transport/stream.h"

namespace net {
namespace http {
class StreamFactory;

namespace protocol {
struct WireRequest;
}  // namespace protocol
}  // namespace http
}  // namespace net

namespace net::http {

/// Stream-building dependencies for Session: the factory plus the
/// transport/TLS options forwarded to it. Built once in the composition
/// root (or by the client that owns the Session).
struct SessionEnvironment {
    std::shared_ptr<StreamFactory> factory;
    Transport::Options transport_opts;
    Transport::TlsOptions tls_opts;
};

/// The origin (scheme/host/port) a Session is bound to; the transport
/// stream is built against it on every (re)connect.
struct SessionOrigin {
    std::string scheme;
    std::string host;
    std::uint16_t port;
};

class Session {
public:
    Session(SessionEnvironment env, SessionOrigin origin, Limits limits);

    /// Perform one request-response exchange over the persistent
    /// connection. Thread-safe.  Cancellation is operation-scoped via
    /// `token` (see Transport::Stream).
    [[nodiscard]] std::expected<Response, Error> exchange(const protocol::WireRequest& req,
                                                          const Utils::CancellationToken& token);

private:
    [[nodiscard]] std::expected<protocol::RawResponse, Error> do_exchange(const protocol::WireRequest& req,
                                                                          const Utils::CancellationToken& token);
    [[nodiscard]] std::expected<void, Error> ensure_stream(const Utils::CancellationToken& token);

    SessionEnvironment env_;
    SessionOrigin origin_;
    Limits limits_;
    std::mutex mutex_;
    std::unique_ptr<Transport::Stream> stream_;
    std::string pending_;  ///< Bytes read past the current response boundary.
    std::optional<unsigned> keep_alive_remaining_;
    std::optional<std::chrono::steady_clock::time_point> keep_alive_deadline_;
};

}  // namespace net::http

#endif  // YADDNSC_HTTP_CLIENT_SESSION_H
