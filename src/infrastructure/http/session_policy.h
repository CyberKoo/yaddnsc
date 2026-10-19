//
// http — connection policy for a persistent session.
//

#ifndef YADDNSC_INFRASTRUCTURE_HTTP_SESSION_POLICY_H
#define YADDNSC_INFRASTRUCTURE_HTTP_SESSION_POLICY_H

#include <optional>

#include "coro/time.h"
#include "infrastructure/http/types.h"  // IWYU pragma: keep — compiled standalone as a header check, so the include must stay

namespace http {

/// One connection's keep-alive budget and its lost-connection replay rule.
///
/// The protocol layer reports what the peer did (ResponseFraming); this unit is
/// the only place that decides whether the connection survives an exchange or
/// whether a lost one may be repaired by sending the request again.
///
/// Failure: none — every decision here is a predicate over framing facts, the
/// request's own policy and the accumulated budget.
/// Thread safety: not thread-safe on its own; it is owned by a Session and used
/// under that session's lock.
class SessionPolicy {
public:
    /// Whether a lost connection may be recovered from by sending the request
    /// again. An idempotent method (RFC 9110 §9.2.2) may always be replayed on a
    /// rebuilt connection; an explicitly replay-safe request also when the peer
    /// may already have processed it on the reused one.
    [[nodiscard]] static bool may_replay(Method method, bool replay_safe, bool reused) noexcept;

    /// Fold a response's keep-alive parameters into the budget and extend its
    /// deadline from the loop clock reading at `now`.
    void record(const ResponseFraming& framing, coro::TimePoint now) noexcept;

    /// Whether the connection may carry another request: the peer's framing
    /// permits it, the request did not ask to close, and the budget is not spent.
    [[nodiscard]] bool reusable(const ResponseFraming& framing, bool request_closes) const noexcept;

    /// Whether the keep-alive deadline has passed at `now` (loop time).
    [[nodiscard]] bool expired(coro::TimePoint now) const noexcept;

    /// Forget the budget and deadline of a connection that is being dropped.
    void reset() noexcept;

private:
    /// Requests the peer still allows on this connection; absent when unbounded.
    std::optional<unsigned> remaining_;
    /// Idle window the peer granted; checked on reuse, never by a timer.
    std::optional<coro::TimePoint> deadline_;
};

}  // namespace http

#endif  // YADDNSC_INFRASTRUCTURE_HTTP_SESSION_POLICY_H
