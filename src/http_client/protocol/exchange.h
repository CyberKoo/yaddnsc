//
// Full HTTP/1.x request-response exchange over a Transport::Stream.
//
// The protocol layer is transport-agnostic and cancellation-agnostic:
// cancellation surfaces as IoError::CANCELLED from the stream and is
// mapped to Error{ErrorCode::CANCELLED, ...} here.
//

#ifndef YADDNSC_HTTP_CLIENT_PROTOCOL_EXCHANGE_H
#define YADDNSC_HTTP_CLIENT_PROTOCOL_EXCHANGE_H

#include <map>
#include <span>
#include <string>
#include <string_view>

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
    HttpVersion version{HttpVersion::V1_1};
    bool reusable{false};
    std::optional<unsigned> keep_alive_max;
    std::optional<unsigned> keep_alive_timeout;
    std::multimap<std::string, std::string> headers;
    std::multimap<std::string, std::string> trailers;
    std::string body;

    /// The body viewed as text (no encoding conversion).
    [[nodiscard]] std::string_view text() const noexcept {
        return body;
    }

    /// The body viewed as raw octets.
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept {
        return {reinterpret_cast<const std::uint8_t *>(body.data()), body.size()};
    }

    /// Body size in octets.
    [[nodiscard]] std::size_t size() const noexcept {
        return body.size();
    }
};

/// Map a transport-level I/O error to a domain error.
[[nodiscard]] Error map_io_error(Transport::IoError err, std::string_view stage);

/// Perform a complete request-response exchange. The returned response records
/// whether the peer permitted reusing the connection for another request.
///
/// Does NOT connect: the caller owns lifecycle (Stream::ensure_connected /
/// close). Safe to call on an already-connected stream only.
[[nodiscard]] std::expected<RawResponse, Error> exchange(Transport::Stream& stream,
                                                         const WireRequest& req,
                                                         const Limits& limits,
                                                         std::string& pending);

/// One-shot convenience overload. Persistent callers must retain `pending`
/// between exchanges so bytes read past one response remain available for the
/// next response.
[[nodiscard]] std::expected<RawResponse, Error> exchange(Transport::Stream& stream,
                                                         const WireRequest& req,
                                                         const Limits& limits);

}  // namespace net::http::protocol

#endif  // YADDNSC_HTTP_CLIENT_PROTOCOL_EXCHANGE_H
