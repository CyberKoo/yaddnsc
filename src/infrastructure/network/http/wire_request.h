//
// Shared request-building helpers for the net::http clients.
//
// Converts a public Request plus origin info into a protocol::WireRequest
// (Host / User-Agent / Content-Length / Content-Type resolved here, so both
// the transient and the persistent client format requests identically).
//

#ifndef YADDNSC_HTTP_CLIENT_WIRE_REQUEST_H
#define YADDNSC_HTTP_CLIENT_WIRE_REQUEST_H

#include <cstdint>
#include <string>
#include <string_view>
#include <expected>

#include "infrastructure/network/http/error.h"
#include "infrastructure/network/http/protocol/wire.h"

class Uri;

namespace Transport {
enum class IoError;
}  // namespace Transport

namespace net {
namespace http {
struct Options;
struct Request;
}  // namespace http
}  // namespace net

namespace net::http {

[[nodiscard]] std::uint16_t default_port(std::string_view scheme) noexcept;

/// Host header value per RFC 7230 §5.4 (bracket IPv6, omit default port).
[[nodiscard]] std::string make_host_header(std::string_view scheme, std::string_view host, std::uint16_t port);

/// path + query for the request line ("/" when empty).
[[nodiscard]] std::string make_target(const Uri& uri);

/// Validate public request fields before serialisation. Header values cannot
/// contain CR/LF and framing/routing headers are normalized by the builder.
[[nodiscard]] std::expected<void, Error> validate_request(const Request& req);

/// Build the wire request: user headers + normalized Host / Connection /
/// User-Agent / Content-Length / Content-Type. The target is filled by the
/// caller. User-supplied Host, Content-Length, Connection, Transfer-Encoding,
/// Trailer, and Upgrade fields are discarded; this client owns framing and
/// connection semantics.
[[nodiscard]] protocol::WireRequest build_wire_request(const Request& req,
                                                       std::string_view scheme,
                                                       std::string_view host,
                                                       std::uint16_t port,
                                                       const Options& opts);

/// Map a transport connect/handshake error to a domain error.
[[nodiscard]] Error map_connect_error(Transport::IoError err);

}  // namespace net::http

#endif  // YADDNSC_HTTP_CLIENT_WIRE_REQUEST_H
