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
#include <string>

#include <expected>

#include "infrastructure/network/http/error.h"
#include "infrastructure/network/http/protocol/exchange.h"
#include "infrastructure/network/http/stream_factory.h"
#include "infrastructure/network/http/types.h"

namespace net::http {

class Session {
public:
    Session(std::shared_ptr<StreamFactory> factory,
            Transport::Options transport_opts,
            Transport::TlsOptions tls_opts,
            std::string scheme,
            std::string host,
            std::uint16_t port,
            Limits limits);

    /// Perform one request-response exchange over the persistent
    /// connection. Thread-safe.
    [[nodiscard]] std::expected<Response, Error> exchange(const protocol::WireRequest& req);

private:
    [[nodiscard]] std::expected<protocol::RawResponse, Error> do_exchange(const protocol::WireRequest& req);
    [[nodiscard]] std::expected<void, Error> ensure_stream();

    std::shared_ptr<StreamFactory> factory_;
    Transport::Options transport_opts_;
    Transport::TlsOptions tls_opts_;
    std::string scheme_;
    std::string host_;
    std::uint16_t port_;
    Limits limits_;
    std::mutex mutex_;
    std::unique_ptr<Transport::Stream> stream_;
    std::string pending_;  ///< Bytes read past the current response boundary.
    std::optional<unsigned> keep_alive_remaining_;
    std::optional<std::chrono::steady_clock::time_point> keep_alive_deadline_;
};

}  // namespace net::http

#endif  // YADDNSC_HTTP_CLIENT_SESSION_H
