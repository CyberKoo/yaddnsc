//
// http — full HTTP/1.x request-response exchange over a net::Stream.
//
// The protocol layer is transport-agnostic: it drives any net::Stream, so a
// test can script an in-memory stream. Cancellation is the awaiting task's cancel
// scope and surfaces as net::IoError::CANCELLED, mapped to ErrorCode::CANCELLED
// here. There is no timeout parameter.
//

#ifndef YADDNSC_HTTP_PROTOCOL_EXCHANGE_H
#define YADDNSC_HTTP_PROTOCOL_EXCHANGE_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <expected>

#include "infrastructure/coro/task.hpp"
#include "infrastructure/net/http/error.h"
#include "infrastructure/net/http/types.h"
#include "infrastructure/net/stream.h"

namespace http::protocol {

struct WireRequest;

/// A decoded response: status, headers (original casing), trailers and body.
///
/// Ownership: owns the body; text()/bytes() are views into it, valid for the
/// lifetime of this value.
struct RawResponse {
    int status{0};
    HttpVersion version{HttpVersion::V1_1};
    /// The peer's framing permits another request on this connection.
    bool reusable{false};
    std::optional<unsigned> keep_alive_max;
    std::optional<unsigned> keep_alive_timeout;
    std::multimap<std::string, std::string> headers;
    std::multimap<std::string, std::string> trailers;
    std::string body;

    /// The body as text (no encoding conversion).
    [[nodiscard]] std::string_view text() const noexcept { return body; }

    /// The body as raw octets.
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept {
        return {reinterpret_cast<const std::uint8_t*>(body.data()), body.size()};
    }

    /// Body size in octets.
    [[nodiscard]] std::size_t size() const noexcept { return body.size(); }
};

/// Perform one request-response exchange on an already-connected stream.
///
/// Does not connect: the caller owns stream lifecycle. `pending` carries bytes
/// read past the previous response so a keep-alive stream can serve several
/// exchanges; it is updated in place.
///
/// Lifetime: the coroutine frame borrows `stream`, `request` and `pending`, so
/// all three must outlive the returned task (they do when the task is awaited
/// inline, which is the only supported use).
/// Failure: expected<RawResponse, Error>; a transport cancellation is
/// CANCELLED, a peer close or socket error is CONNECTION_LOST.
[[nodiscard]] coro::Task<std::expected<RawResponse, Error>> exchange(net::Stream& stream, const WireRequest& request,
                                                                     const Limits& limits, std::string& pending);

/// One-shot overload for callers that own the whole connection.
[[nodiscard]] coro::Task<std::expected<RawResponse, Error>> exchange(net::Stream& stream, const WireRequest& request,
                                                                     const Limits& limits);

}  // namespace http::protocol

#endif  // YADDNSC_HTTP_PROTOCOL_EXCHANGE_H
