//
// Coroutine runtime — the host extension surface.
//
// detail/access.h is the runtime's own door into Loop and CancelScope. This
// header is the second, narrower door for host code that bridges foreign
// threads into the runtime. Waiting and notification are covered by the public
// coro::Notification; what a host still needs here is what ordinary callers
// never see — observing one scope's cancellation while waiting on another, and
// a group lifetime token — as opaque primitives that cannot corrupt the waiter
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
#include "coro/detail/access.h"       // IWYU pragma: export
#include "coro/detail/group_state.h"  // IWYU pragma: export
#include "coro/detail/wait_node.h"    // IWYU pragma: export
#include "coro/task_group.hpp"

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
    CancelObserver(CancelScope& scope, void* owner, void (*on_cancel)(void*) noexcept) noexcept
        : on_cancel_(on_cancel), owner_(owner) {
        assert(on_cancel != nullptr && "arming a callbackless CancelObserver");
        if (scope.cancelled()) {
            on_cancel(owner);
            return;
        }
        node_.owner = this;
        node_.on_cancel = &CancelObserver::dispatch;
        node_.wake_on_cancel = false;  // pure observation: no frame to wake
        detail::ScopeAccess::add_waiter(scope, node_);
    }

    /// Unregisters the hook if still armed.
    ~CancelObserver() noexcept { disarm(); }

    CancelObserver(const CancelObserver&) = delete;
    CancelObserver& operator=(const CancelObserver&) = delete;
    CancelObserver(CancelObserver&&) = delete;
    CancelObserver& operator=(CancelObserver&&) = delete;

    /// Unlink from the scope. Idempotent: safe after the scope already fired.
    void disarm() noexcept {
        if (node_.linked) {
            detail::ScopeAccess::remove_waiter(*node_.scope, node_);
        }
    }

    /// True while registered on a scope.
    [[nodiscard]] bool armed() const noexcept { return node_.linked; }

private:
    static void dispatch(detail::WaitNode& node) noexcept {
        auto& self = *static_cast<CancelObserver*>(node.owner);
        self.on_cancel_(self.owner_);
    }

    detail::WaitNode node_{};
    void (*on_cancel_)(void*) noexcept = nullptr;
    void* owner_ = nullptr;
};

/// Lifetime token for a TaskGroup, captured by the host and carried into
/// posted loop callbacks.
///
/// A posted callback must never retain a raw TaskGroup beyond the group's body
/// lifetime; it carries this token instead and reads it on the loop, where a
/// null result means the group has exited and ingress is closed.
class GroupToken {
public:
    GroupToken() = default;

    /// The live group, or null once it has exited. Loop thread only.
    [[nodiscard]] TaskGroup* get() const noexcept { return lifetime_ != nullptr ? lifetime_->group : nullptr; }

    /// True when the token was captured from a group; says nothing about
    /// whether the group is still alive — use get() for that.
    [[nodiscard]] explicit operator bool() const noexcept { return lifetime_ != nullptr; }

private:
    friend GroupToken group_token(TaskGroup&) noexcept;

    explicit GroupToken(std::shared_ptr<detail::GroupLifetime> lifetime) noexcept : lifetime_(std::move(lifetime)) {}

    std::shared_ptr<detail::GroupLifetime> lifetime_;
};

/// Capture `group`'s lifetime token. Loop thread only.
[[nodiscard]] inline GroupToken group_token(TaskGroup& group) noexcept {
    return GroupToken(detail::GroupAccess::lifetime(group));
}

}  // namespace coro

#endif  // YADDNSC_INFRASTRUCTURE_CORO_HOST_HPP
