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
#include <string>
#include <string_view>

#include <expected>

#include "infrastructure/http/error.h"
#include "infrastructure/http/protocol/wire.h"
#include "infrastructure/http/types.h"

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

/// Build a wire request: user headers plus the client-owned Host, Connection,
/// User-Agent, Content-Length and Content-Type. The target is filled by the
/// caller.
[[nodiscard]] protocol::WireRequest build_wire_request(const Request& request, std::string_view scheme,
                                                       std::string_view host, std::uint16_t port,
                                                       const Options& options);

/// Inverse of build_wire_request: rebuild a public Request from a wire request so
/// a redirect hop can be replayed through another origin. Every client-managed
/// header is dropped regardless of casing; Content-Type is carried into
/// Request::content_type.
[[nodiscard]] Request to_public_request(const protocol::WireRequest& wire);

/// Construct an I/O failure in the HTTP error vocabulary.
[[nodiscard]] Error connection_error(std::string_view stage);

/// Construct a connect failure. The stream bundles TCP connect and TLS handshake
/// into one call, so no phase is distinguished — the legacy stack did the
/// same and reported both as CONNECT_FAILED.
[[nodiscard]] Error connect_error();

}  // namespace http

#endif  // YADDNSC_INFRASTRUCTURE_HTTP_WIRE_REQUEST_H
