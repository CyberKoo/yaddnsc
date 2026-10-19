#ifndef YADDNSC_INFRASTRUCTURE_HTTP_REDIRECT_H
#define YADDNSC_INFRASTRUCTURE_HTTP_REDIRECT_H

#include <map>
#include <optional>
#include <string>

#include "infrastructure/http/types.h"

class Uri;

namespace http {

/// Logical follow-up request. Wire framing belongs exclusively to the builder.
struct RedirectPlan {
    Request next;
    std::string url;
    bool cross_origin{false};
};

struct RedirectEval {
    std::optional<RedirectPlan> plan;
    bool limit_reached{false};
};

/// Pure redirect decision: 307/308 preserve method/body; 301/302/303 use GET.
/// Cross-origin hops remove authentication/cookies; TLS downgrades are refused.
[[nodiscard]] RedirectEval evaluate_redirect(int status, const std::multimap<std::string, std::string>& headers,
                                             int redirect_count, const Options& options, const Request& current,
                                             const Uri& current_uri);

}  // namespace http

#endif  // YADDNSC_INFRASTRUCTURE_HTTP_REDIRECT_H
