//
// http — redirect policy.
//
// Pure decision logic: given a 3xx response, produce the follow-up request or
// the decision not to follow. No I/O.
//

#ifndef YADDNSC_INFRASTRUCTURE_NET_HTTP_REDIRECT_H
#define YADDNSC_INFRASTRUCTURE_NET_HTTP_REDIRECT_H

#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "infrastructure/http/protocol/wire.h"
#include "infrastructure/http/types.h"

class Uri;

namespace http {

/// A resolved redirect target plus the request to send there.
struct RedirectPlan {
    protocol::WireRequest next;  ///< Follow-up request (Host header set for the target).
    std::string scheme;          ///< Target scheme ("http" / "https").
    std::string host;            ///< Target host, for the next connection.
    std::uint16_t port{0};       ///< Target port (default-filled).
    bool cross_origin{false};    ///< scheme/host/port differ from the current request.
};

/// Outcome of evaluating a response for redirection.
struct RedirectEval {
    std::optional<RedirectPlan> plan;  ///< Follow this redirect.
    bool limit_reached{false};         ///< The redirect budget is exhausted — fail.
};

/// Evaluate a received response for redirection.
///
/// Follows RFC 7231 §6.4 with these rules:
///   - 307/308 preserve the method and body; 301/302/303 rewrite to GET and drop
///     the body.
///   - Authorization / Cookie / Proxy-Authorization are dropped when the
///     redirect crosses an origin (scheme, host or port change).
///   - An https -> http downgrade is never followed.
[[nodiscard]] RedirectEval evaluate_redirect(int status, const std::multimap<std::string, std::string>& headers,
                                             int redirect_count, const Options& options,
                                             const protocol::WireRequest& current, const Uri& current_uri);

}  // namespace http

#endif  // YADDNSC_INFRASTRUCTURE_NET_HTTP_REDIRECT_H
