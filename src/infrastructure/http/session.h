//
// http — persistent session over one reused connection.
//
// A session is bound to one origin and keeps its connection alive across
// exchanges (keep-alive count and deadline included). Concurrent callers are
// serialized by an AsyncMutex — the waiting semantics of the redesign, not the
// legacy "overlapping call is an error" rejection.
//

#ifndef YADDNSC_INFRASTRUCTURE_HTTP_SESSION_H
#define YADDNSC_INFRASTRUCTURE_HTTP_SESSION_H

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <expected>

#include "infrastructure/coro/async_mutex.hpp"
#include "infrastructure/coro/task.hpp"
#include "infrastructure/http/error.h"
#include "infrastructure/http/protocol/exchange.h"
#include "infrastructure/http/types.h"

namespace net {
class Stream;
}  // namespace net

namespace http {
namespace protocol {
struct WireRequest;
}  // namespace protocol

/// One origin's persistent connection.
///
/// Ownership: owns the stream and the pending read buffer; close() releases the
/// connection but the session stays usable (the next exchange reconnects).
/// Failure: expected<Response, Error>. A lost connection is retried once for an
/// idempotent request (RFC 9110 §9.2.2); any other failure is reported as is.
/// Cancellation: a cancelled wait on the session lock or on the connection is
/// observed by throwing `coro::Cancelled`.
/// Thread safety: not thread-safe as an object, but concurrent exchanges are
/// serialized by the session's own mutex, so the *connection* is never used by
/// two coroutines at once. The same holds for close(): while an exchange holds
/// the mutex the close is deferred to a guard checkpoint instead of tearing
/// down the borrowed stream mid-exchange. Do not move a session while an
/// exchange is in flight.
class Session {
public:
    /// @param options  Client options (transport, TLS, limits, resolution).
    /// @param scheme   "http" or "https".
    /// @param host     Origin host (IP literal or resolvable name).
    /// @param port     Origin port.
    Session(Options options, std::string scheme, std::string host, std::uint16_t port);

    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    /// Perform one exchange on the reused connection.
    ///
    /// `target` is path + query ("/v1/update?foo=bar"); it is used verbatim, so
    /// a session is only ever pointed at its own origin.
    [[nodiscard]] coro::Task<std::expected<Response, Error>> exchange(std::string target, const Request& request);

    /// Drop the connection; the session reconnects on the next exchange. An
    /// exchange in flight is left alone: the close is deferred to the session
    /// guard's checkpoint and drops the connection as soon as the exchange
    /// completes.
    void close() noexcept;

    [[nodiscard]] const std::string& scheme() const noexcept { return scheme_; }

    [[nodiscard]] const std::string& host() const noexcept { return host_; }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    [[nodiscard]] coro::Task<std::expected<protocol::RawResponse, Error>> do_exchange(
        const protocol::WireRequest& wire);

    [[nodiscard]] coro::Task<std::expected<void, Error>> ensure_stream();

    void drop_connection() noexcept;

    Options options_;
    std::string scheme_;
    std::string host_;
    std::uint16_t port_{0};
    coro::AsyncMutex mutex_;
    std::unique_ptr<net::Stream> stream_;
    /// A close() that arrived while an exchange held the mutex.
    bool close_requested_ = false;
    /// Bytes read past the previous response boundary.
    std::string pending_;
    std::optional<unsigned> keep_alive_remaining_;
    std::optional<std::chrono::steady_clock::time_point> keep_alive_deadline_;
};

}  // namespace http

#endif  // YADDNSC_INFRASTRUCTURE_HTTP_SESSION_H
