//
// http — name resolution and stream connection shared by the clients.
//
// The HTTP layer never touches a socket directly: it resolves the URL host
// through the configured bootstrap DNS, asks the injected StreamFactory (or the
// production one) for a stream, and lets the stream connect. That keeps the fake
// injection point in one place.
//

#ifndef YADDNSC_HTTP_TRANSPORT_H
#define YADDNSC_HTTP_TRANSPORT_H

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <expected>

#include "domain/network/inet_address.h"
#include "infrastructure/coro/task.hpp"
#include "infrastructure/net/http/error.h"
#include "infrastructure/net/http/types.h"
#include "infrastructure/net/stream.h"

namespace http {

/// Resolve `host` to candidate addresses: an IP literal short-circuits, anything
/// else goes through the configured bootstrap DNS servers.
///
/// Failure: RESOLVE_FAILED when resolution fails, CONFIG when no bootstrap
/// servers are configured for a hostname. The coroutine frame copies `host` and
/// the server list, so nothing needs to outlive the call.
[[nodiscard]] coro::Task<std::expected<std::vector<InetAddress>, Error>> resolve_host(std::string host,
                                                                                      const Options& options);

/// Connect to the first candidate address that accepts, in order.
///
/// `tls` selects the TLS or the plain-TCP stream from the factory. `options`
/// and `addresses` are borrowed for the duration of the task, which is awaited
/// inline by the caller's own frame.
/// Failure: CONNECT_FAILED / TLS_HANDSHAKE_FAILED for the last attempt,
/// CANCELLED as soon as the scope is cancelled.
[[nodiscard]] coro::Task<std::expected<std::unique_ptr<net::Stream>, Error>> connect_stream(
    std::string_view scheme, std::span<const InetAddress> addresses, std::uint16_t port, const Options& options);

}  // namespace http

#endif  // YADDNSC_HTTP_TRANSPORT_H
