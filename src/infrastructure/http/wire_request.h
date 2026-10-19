//
// http — request building for the client.
//
// A public Request plus origin information becomes a protocol::WireRequest here,
// so the transient client and the persistent session format requests
// identically. The client owns framing and routing: user-supplied Host,
// Content-Length, Content-Type, Connection and User-Agent fields are dropped,
// and Content-Type on the wire comes only from Request::content_type.
//

#ifndef YADDNSC_INFRASTRUCTURE_HTTP_WIRE_REQUEST_H
#define YADDNSC_INFRASTRUCTURE_HTTP_WIRE_REQUEST_H

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "infrastructure/http/error.h"
#include "infrastructure/http/protocol/wire.h"
#include "infrastructure/http/types.h"  // IWYU pragma: keep — compiled standalone as a header check, so the include must stay

class Uri;

namespace http {

/// Default port for a scheme (443 for https, 80 otherwise).
[[nodiscard]] std::uint16_t default_port(std::string_view scheme) noexcept;

/// Host header value per RFC 7230 §5.4: bracket an IPv6 literal, omit a
/// default port.
[[nodiscard]] std::string make_host_header(std::string_view scheme, std::string_view host, std::uint16_t port);

/// path + query for the request line ("/" when the path is empty).
[[nodiscard]] std::string make_target(const Uri& uri);

/// Validate user-supplied request fields before serialization.
[[nodiscard]] std::expected<void, Error> validate_request(const Request& request);

/// Validate a built wire request: the request-target grammar and the
/// framing/routing headers that will be serialized. This is the single
/// request-side conclusion the protocol layer consumes — it treats a wire
/// request that passed here as valid.
[[nodiscard]] std::expected<void, Error> validate_wire_request(const protocol::WireRequest& wire);

/// Whether a request built with these options asks the peer to close the
/// connection once the exchange completes.
[[nodiscard]] bool closes_connection(const Options& options) noexcept;

/// Build a wire request: user headers plus the client-owned Host, Connection,
/// User-Agent, Content-Length and Content-Type. The target is filled by the
/// caller and must be checked with validate_wire_request().
[[nodiscard]] protocol::WireRequest build_wire_request(const Request& request, std::string_view scheme,
                                                       std::string_view host, std::uint16_t port,
                                                       const Options& options);

/// Reassemble a validated HTTP origin and target, bracketing IPv6 hosts.
[[nodiscard]] std::string make_absolute_url(std::string_view scheme, std::string_view host, std::uint16_t port,
                                            std::string_view target);

}  // namespace http

#endif  // YADDNSC_INFRASTRUCTURE_HTTP_WIRE_REQUEST_H
