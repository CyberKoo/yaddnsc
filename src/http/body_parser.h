//
// Created by Kotarou on 2026/7/18.
//

#ifndef YADDNSC_HTTP_BODY_PARSER_H
#define YADDNSC_HTTP_BODY_PARSER_H

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "http/types.h"
#include "network/transport/stream.h"
#include "util/cancellation_token.hpp"

namespace Http {

/// Read and decode the HTTP response body after headers have been parsed.
///
/// Handles both Content-Length and Transfer-Encoding: chunked bodies.
/// This function is transport-agnostic — it reads through the @p stream
/// interface and does not know whether the underlying transport is TLS,
/// plain TCP, QUIC, or a test mock.
///
/// @param stream           Transport stream to read remaining body data from.
/// @param headers          Parsed response headers (from parse_response).
/// @param header_buf       The raw buffer that was passed to parse_response.
///                         It may already contain some body data after the
///                         header section.
/// @param max_body_size    Maximum allowed body size.
/// @param cancel_token     Cancellation signal forwarded to transport reads.
///
/// @return  The decoded response body, or an Error describing the failure.
[[nodiscard]] std::expected<std::vector<std::uint8_t>, Error> read_body(
    Transport::Stream &stream,
    const ResponseHeaders &headers,
    std::span<const char> header_buf,
    size_t max_body_size,
    const Utils::CancellationToken &cancel_token);

}  // namespace Http

#endif  // YADDNSC_HTTP_BODY_PARSER_H
