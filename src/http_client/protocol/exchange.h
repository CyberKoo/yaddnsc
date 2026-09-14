//
// Full HTTP/1.1 request-response exchange over a Transport::Stream.
//
// The protocol layer is transport-agnostic and cancellation-agnostic:
// cancellation surfaces as IoError::CANCELLED from the stream and is
// mapped to Error{ErrorCode::CANCELLED, ...} here.
//

#ifndef YADDNSC_HTTP_CLIENT_PROTOCOL_EXCHANGE_H
#define YADDNSC_HTTP_CLIENT_PROTOCOL_EXCHANGE_H

#include <map>
#include <string>

#include <expected>

#include "http_client/error.h"
#include "http_client/protocol/wire.h"
#include "http_client/types.h"
#include "network/transport/stream.h"

namespace net::http::protocol {

/// Parsed HTTP response: status, headers (original casing preserved) and
/// body as raw bytes.
struct RawResponse {
    int status;
    std::multimap<std::string, std::string> headers;
    std::string body;
};

/// Map a transport-level I/O error to a domain error.
[[nodiscard]] Error map_io_error(Transport::IoError err, std::string_view stage);

/// Perform a complete request-response exchange.
///
/// Does NOT connect: the caller owns lifecycle (Stream::ensure_connected /
/// close). Safe to call on an already-connected stream only.
[[nodiscard]] std::expected<RawResponse, Error> exchange(Transport::Stream& stream,
                                                         const WireRequest& req,
                                                         const Limits& limits);

}  // namespace net::http::protocol

#endif  // YADDNSC_HTTP_CLIENT_PROTOCOL_EXCHANGE_H
