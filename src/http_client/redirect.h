//
// Redirect policy for the net::http client domain.
//
// Pure decision logic: given a 3xx response, produce the follow-up request
// (or the decision not to follow). No I/O.
//

#ifndef YADDNSC_HTTP_CLIENT_REDIRECT_H
#define YADDNSC_HTTP_CLIENT_REDIRECT_H

#include <cstdint>
#include <optional>
#include <string>

#include "http/error.h"
#include "http_client/protocol/exchange.h"
#include "http/types.h"

#include "uri.h"

namespace net::http {

/// A resolved redirect target plus the request to send there.
struct RedirectPlan {
    protocol::WireRequest next;  ///< Follow-up request (Host header set for the target).
    std::string scheme;          ///< Target scheme ("http" / "https").
    std::string host;            ///< Target host (for the connection).
    std::uint16_t port;          ///< Target port (default-filled).
    bool cross_origin;           ///< scheme/host/port differ from the current request.
};

/// Result of evaluating a response for redirection.
struct RedirectEval {
    std::optional<RedirectPlan> plan;  ///< Follow this redirect.
    bool limit_reached = false;        ///< Redirect limit exceeded — fail the exchange.
};

/// Evaluate a received response for redirection.
///
/// Follows RFC 7231 §6.4 with these rules:
///   - 307/308 preserve the method and body; 301/302/303 rewrite to GET
///     and drop the body (common safe practice).
///   - Authorization / Cookie / Proxy-Authorization headers are dropped
///     when the redirect crosses an origin (scheme, host, or port change).
[[nodiscard]] RedirectEval evaluate_redirect(int status,
                                             const std::multimap<std::string, std::string>& headers,
                                             int redirect_count,
                                             const Options& opts,
                                             const protocol::WireRequest& current,
                                             const Uri& current_uri);

}  // namespace net::http

#endif  // YADDNSC_HTTP_CLIENT_REDIRECT_H
