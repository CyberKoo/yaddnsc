//
// http — connection policy for a persistent session.
//

#include "session_policy.h"

#include <algorithm>
#include <chrono>
#include <compare>

#include "infrastructure/http/types.h"

namespace http {
namespace {

/// Methods safe to replay after a rebuilt connection (RFC 9110 §9.2.2).
[[nodiscard]] bool is_idempotent(const Method method) noexcept {
    return method == Method::GET || method == Method::HEAD || method == Method::OPTIONS || method == Method::PUT ||
           method == Method::DELETE;
}

}  // namespace

bool SessionPolicy::may_replay(const Method method, const bool replay_safe, const bool reused) noexcept {
    return is_idempotent(method) || (replay_safe && reused);
}

void SessionPolicy::record(const ResponseFraming& framing, const coro::TimePoint now) noexcept {
    if (framing.max.has_value()) {
        // `max` caps the requests on this connection; a repeated header must not
        // reset a budget earlier exchanges already consumed.
        remaining_ = remaining_.has_value() ? std::min(*remaining_, *framing.max) : framing.max;
    }
    if (framing.timeout.has_value()) {
        deadline_ = now + std::chrono::seconds(*framing.timeout);
    }
    if (remaining_.has_value() && *remaining_ > 0) {
        --*remaining_;
    }
}

bool SessionPolicy::reusable(const ResponseFraming& framing, const bool request_closes) const noexcept {
    if (request_closes || framing.connection_close || !framing.self_delimiting) {
        return false;
    }
    if (framing.version != HttpVersion::V1_1 && !framing.keep_alive) {
        return false;
    }
    return !remaining_.has_value() || *remaining_ > 0;
}

bool SessionPolicy::expired(const coro::TimePoint now) const noexcept {
    return deadline_.has_value() && now >= *deadline_;
}

void SessionPolicy::reset() noexcept {
    remaining_.reset();
    deadline_.reset();
}

}  // namespace http
