//
// Coroutine runtime — the host extension surface.
//
// detail/access.h is the runtime's own door into Loop and CancelScope. This
// header is the second, narrower door for host code that bridges foreign
// threads into the runtime. Waiting and notification are covered by the public
// coro::Notification; what a host still needs here is what ordinary callers
// never see — observing one scope's cancellation while waiting on another, and
// a nursery lifetime token — as opaque primitives that cannot corrupt the waiter
// bookkeeping.
//
// Thread safety: every primitive here is loop-thread only, like the scope tree
// they register on. A foreign thread communicates through the host's own
// shared state (atomics, promises) and never touches these objects directly.
//

#ifndef YADDNSC_INFRASTRUCTURE_CORO_HOST_HPP
#define YADDNSC_INFRASTRUCTURE_CORO_HOST_HPP

#include <cassert>
#include <memory>
#include <utility>

#include "coro/cancel_scope.h"
#include "coro/detail/access.h"           // IWYU pragma: export
#include "coro/detail/nursery_state.hpp"  // IWYU pragma: export
#include "coro/detail/wait_operation.h"   // IWYU pragma: export
#include "coro/nursery.hpp"

namespace coro {

/// RAII cancellation hook on a scope, for hosts that must observe one scope
/// while their coroutine waits on another (for example inside a shield).
///
/// Construction registers the hook; the scope runs it once from cancel() on
/// the loop thread — inline during construction when the scope is already
/// cancelled. Destruction unregisters it. The hook must not throw, block,
/// destroy scopes, or re-enter the runtime — it is a bookkeeping callback:
/// update state, post a notification, nothing else.
class CancelObserver {
public:
    /// Register on `scope`: on cancellation, run `on_cancel(owner)` once on the
    /// loop thread. An already-cancelled scope runs the callback immediately,
    /// during construction, and nothing is registered.
    /// This is a pure subscription: it creates no waiter and no frame resume
    /// target, so arming on a cancelled scope notifies inline instead of
    /// throwing, and it never keeps a cancelled operation alive.
    CancelObserver(CancelScope& scope, void* owner, void (*on_cancel)(void*) noexcept) noexcept
        : on_cancel_(on_cancel), owner_(owner) {
        assert(on_cancel != nullptr && "arming a callbackless CancelObserver");
        subscription_.arm(scope, this, &CancelObserver::dispatch);
    }

    /// Unregisters the hook if still armed.
    ~CancelObserver() noexcept { disarm(); }

    CancelObserver(const CancelObserver&) = delete;
    CancelObserver& operator=(const CancelObserver&) = delete;
    CancelObserver(CancelObserver&&) = delete;
    CancelObserver& operator=(CancelObserver&&) = delete;

    /// Unlink from the scope. Idempotent: safe after the scope already fired.
    void disarm() noexcept { subscription_.disarm(); }

    /// True while registered on a scope.
    [[nodiscard]] bool armed() const noexcept { return subscription_.armed(); }

private:
    /// The subscription's callback shape also carries a resume action; pure
    /// observation has none, so it never asks for a resume.
    static detail::CancelAction dispatch(void* context) noexcept {
        auto& self = *static_cast<CancelObserver*>(context);
        self.on_cancel_(self.owner_);
        return detail::CancelAction::WAIT_EXTERNAL;
    }

    detail::CancelSubscription subscription_{};
    void (*on_cancel_)(void*) noexcept = nullptr;
    void* owner_ = nullptr;
};

/// Nursery ingress token for posted host callbacks, not an execution owner.
/// Read get() only on the loop: body return or fail-fast closes admission before
/// children drain, so the token may be closed while resources are still alive.
class NurseryToken {
public:
    NurseryToken() = default;

    /// Borrow the nursery while admission is open, otherwise null. Loop only.
    [[nodiscard]] Nursery* get() const noexcept { return lifetime_ != nullptr ? lifetime_->nursery : nullptr; }

    /// Whether captured from a nursery; use get() to check current admission.
    [[nodiscard]] explicit operator bool() const noexcept { return lifetime_ != nullptr; }

private:
    friend NurseryToken nursery_token(Nursery&) noexcept;

    explicit NurseryToken(std::shared_ptr<detail::NurseryLifetime> lifetime) noexcept
        : lifetime_(std::move(lifetime)) {}

    std::shared_ptr<detail::NurseryLifetime> lifetime_;
};

/// Capture the nursery's ingress token without allocating. Loop thread only.
[[nodiscard]] inline NurseryToken nursery_token(Nursery& nursery) noexcept {
    return NurseryToken(detail::NurseryAccess::lifetime(nursery));
}

}  // namespace coro

#endif  // YADDNSC_INFRASTRUCTURE_CORO_HOST_HPP
