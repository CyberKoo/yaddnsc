//
// http — name resolution and stream connection shared by the clients.
//
// The HTTP layer never touches a socket directly: it resolves the URL host
// through the injected hostname resolver, asks the StreamFactory (or the
// production one) for a stream, and lets the stream connect. That keeps the fake
// injection point in one place.
//

#ifndef YADDNSC_INFRASTRUCTURE_NET_HTTP_TRANSPORT_H
#define YADDNSC_INFRASTRUCTURE_NET_HTTP_TRANSPORT_H

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <expected>

#include "domain/network/inet_address.h"
#include "infrastructure/coro/task.hpp"
#include "infrastructure/http/error.h"
#include "infrastructure/http/types.h"
#include "infrastructure/network/transport/stream.h"

namespace http {

/// Resolve `host` to candidate addresses: an IP literal short-circuits, anything
/// else goes through Options::resolve.
///
/// Failure: RESOLVE_FAILED when resolution fails or no resolver is injected.
/// The task owns `host` and borrows `options` until completion; cancellation
/// propagates as coro::Cancelled.
[[nodiscard]] coro::Task<std::expected<std::vector<domain::InetAddress>, Error>> resolve_host(std::string host,
                                                                                              const Options& options);

/// Connect to the first candidate address that accepts, in order.
///
/// `scheme` selects the TLS or the plain-TCP stream from the factory. For
/// https, `host` — the origin name the addresses were resolved from — becomes
/// the TLS identity (SNI and certificate verification) unless
/// Options::tls.sni_hostname pins a name explicitly; an IP-literal host is
/// never turned into SNI (RFC 6066 §3). `options` and `addresses` are borrowed
/// for the duration of the task, which is awaited inline by the caller's own
/// frame.
/// Failure: CONNECT_FAILED for the last attempt; cancellation throws `coro::Cancelled`.
[[nodiscard]] coro::Task<std::expected<std::unique_ptr<net::Stream>, Error>> connect_stream(
    std::string_view scheme, std::string_view host, std::span<const domain::InetAddress> addresses, std::uint16_t port,
    const Options& options);

}  // namespace http

#endif  // YADDNSC_INFRASTRUCTURE_NET_HTTP_TRANSPORT_H
