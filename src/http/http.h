//
// Created by Kotarou on 2026/7/18.
//

#ifndef YADDNSC_HTTP_H
#define YADDNSC_HTTP_H

#include <expected>
#include <string_view>

#include "http/types.h"
#include "http_type.h"

namespace Transport { class Stream; }

namespace Utils { class CancellationToken; }

/// Transport-agnostic HTTP/1.1 client protocol implementation.
///
/// All public functions operate on a @ref Transport::Stream, which
/// abstracts away the underlying transport (TLS, TCP, QUIC, etc.).
namespace Http {

/// Perform a complete HTTP/1.1 request-response exchange.
///
/// Builds the wire-format request, sends it over @p stream, reads and
/// parses the full response.  A single call covers:
///   build_request() → stream.send_all() → read headers + body
///
/// @param stream            Transport stream.
/// @param path              Request path (e.g. "/dns-query").
/// @param req               Request parameters (method, headers, body, content_type).
/// @param host_header       Pre-formatted Host header value.
/// @param user_agent        User-Agent header value.
/// @param cancel_token      Cancellation signal.
///
/// @return  The complete HTTP response, or an error code.
///
/// @par Thread Safety
/// **Not thread-safe.** The @p stream must not be shared across threads
/// without external synchronization.  This function modifies @p stream
/// (sends then reads) and is itself not reentrant.
[[nodiscard]] std::expected<Response, Error> exchange(Transport::Stream& stream,
                                                      std::string_view path,
                                                      const HttpRequest& req,
                                                      std::string_view host_header,
                                                      std::string_view user_agent,
                                                      const Utils::CancellationToken& cancel_token);

}  // namespace Http

#endif  // YADDNSC_HTTP_H
