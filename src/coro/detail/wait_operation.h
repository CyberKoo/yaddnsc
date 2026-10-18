//
// Coroutine runtime — the wait protocol: one subscription, one resume arbitration.
//
// A wait is two separable facts. `CancelSubscription` is the only way to observe a
// scope's cancellation: it owns the membership and the one-shot notification, and
// it knows nothing about frames. `WaitOperation` arbitrates a single wait's
// resume: exactly one of "the event fired" and "the scope was cancelled" may take
// the resume entitlement, and the phase keeps the loser from taking it twice.
//
// They compose but do not imply each other. An inline task await and an internal
// nursery drain use the arbitration without ever arming a subscription (I3/I4);
// a pure observer such as coro::CancelObserver uses the subscription with no
// arbitration at all.
//
// Startup order is fixed by design (5.5): begin, then subscribe — only a
// cancellable event adapter does that — then register the event, then commit.
// A failure anywhere before the commit rolls the operation back; nothing is left
// half-armed.
//
// Thread safety: loop thread only. Cross-thread producers (an offload worker)
// publish through their own inbox node and touch none of this.
//

#ifndef YADDNSC_INFRASTRUCTURE_CORO_DETAIL_WAIT_OPERATION_H
#define YADDNSC_INFRASTRUCTURE_CORO_DETAIL_WAIT_OPERATION_H

#include <cassert>
#include <exception>

#include "coro/detail/intrusive_list.hpp"
#include "coro/fwd.h"

namespace coro::detail {

class CancelSubscription;
class ExecutionRecord;

/// Why a scope was cancelled. Only TIMEOUT is observable through the scope.
enum class CancelCause { REQUESTED, TIMEOUT };

/// What a cancelled wait must do with the resume entitlement.
///
/// RESUME resumes the waiter now; it has already undone whatever the event was
/// holding. WAIT_EXTERNAL leaves the waiter parked: the event is still running,
/// and its own completion will report readiness (a joined worker, for example).
enum class CancelAction { RESUME, WAIT_EXTERNAL };

/// Runtime-only door to a scope's subscription chain, and to the one entry that
/// may throw. Defined where CancelScope is complete (cancel_scope.cpp), so this
/// header needs no knowledge of a scope's layout.
struct SubscriptionAccess {
    /// Arm `subscription` on `scope`, or throw Cancelled first when the scope is
    /// already cancelled. The throw precedes any linking, so a failed start leaves
    /// no subscription behind.
    static void subscribe(CancelScope& scope, CancelSubscription& subscription, void* context,
                          CancelAction (*on_cancel)(void*) noexcept);
};

/// Queue an execution for resumption. Defined in detail/access.h, the runtime's
/// only door into Loop, so this header stays independent of it.
void schedule_resume(ExecutionRecord& record) noexcept;

/// The only subscription to a scope's cancellation.
///
/// The subscriber owns its subscription and keeps it alive until it disarms: a
/// scope holds borrowed links only. Arming on an already-cancelled scope runs the
/// callback inline exactly once and leaves the subscription unarmed, which is
/// what lets a pure observer notify during construction without ever linking.
class CancelSubscription {
public:
    CancelSubscription() noexcept { hook_.bind(*this); }

    /// A subscription outlives nothing: destroying an armed one would leave the
    /// scope holding a link into released storage (Debug: assert, Release:
    /// std::terminate(), the same policy as the intrusive component).
    ~CancelSubscription() noexcept {
        assert(!armed() && "cancelling subscription destroyed while still armed");
        if (armed()) {
            std::terminate();
        }
    }

    CancelSubscription(const CancelSubscription&) = delete;
    CancelSubscription& operator=(const CancelSubscription&) = delete;
    CancelSubscription(CancelSubscription&&) = delete;
    CancelSubscription& operator=(CancelSubscription&&) = delete;

    /// Arm on `scope`. A scope that is already cancelled notifies inline once and
    /// leaves this unarmed. Never throws and never allocates.
    void arm(CancelScope& scope, void* context, CancelAction (*on_cancel)(void*) noexcept) noexcept;

    /// Revoke the subscription. Idempotent, and it does not undo the external
    /// work the subscriber may have started.
    void disarm() noexcept { hook_.unlink(); }

    /// True while linked to a scope.
    [[nodiscard]] bool armed() const noexcept { return hook_.linked(); }

private:
    friend class ::coro::CancelScope;

    /// Runtime-only: pop this subscription off its scope and notify it. The scope
    /// has already unlinked it, which is what makes the notification single-shot.
    void notify() noexcept {
        assert(on_cancel_ != nullptr && "armed subscription without a callback");
        if (on_cancel_ != nullptr) {
            on_cancel_(context_);
        }
    }

    IntrusiveHook<CancelSubscription> hook_;
    void* context_ = nullptr;
    CancelAction (*on_cancel_)(void*) noexcept = nullptr;
};

/// Where one wait stands. The legal transitions are a closed set:
/// IDLE → ARMING → PARKED → QUEUED → RESUMING → FINISHED, plus ARMING → IDLE for
/// a start that failed before the commit. Anything else is a contract violation.
enum class WaitPhase { IDLE, ARMING, PARKED, QUEUED, RESUMING, FINISHED };

/// Resume arbitration for exactly one wait. This half carries no cancellation
/// storage: an inline task await and an internal drain park here directly.
///
/// The waiter is the execution record that parks; the event adapter calls
/// event_ready() when its source completes. Only one of the event and a
/// cancellation may take the resume entitlement, which is why every adapter
/// uses this arbitration even when it never subscribes.
class WaitCore {
public:
    WaitCore() = default;

    WaitCore(const WaitCore&) = delete;
    WaitCore& operator=(const WaitCore&) = delete;
    WaitCore(WaitCore&&) = delete;
    WaitCore& operator=(WaitCore&&) = delete;

    /// Bind the resume target and enter ARMING. Never checks or subscribes
    /// cancellation: a plain `co_await task` and an internal drain must not
    /// become cancellation checkpoints.
    void begin(ExecutionRecord& waiter) noexcept {
        assert(phase_ == WaitPhase::IDLE && "a wait operation is single-shot");
        waiter_ = &waiter;
        phase_ = WaitPhase::ARMING;
    }

    /// The event's only completion entry, callable more than once: a completion
    /// during ARMING is recorded and published at the commit, and one that
    /// arrives after the queueing cannot queue again. A completion that lands
    /// while the waiter is being resumed is dropped, because queueing it would
    /// resume a frame that is not suspended.
    void event_ready() noexcept { claim_resume(); }

    /// Commit the suspension after every fallible registration. A completion that
    /// arrived while arming is published here, which is what keeps a failing
    /// registration from leaving a queued resume behind.
    void commit_suspend() noexcept {
        assert(phase_ == WaitPhase::ARMING && "commit outside ARMING");
        phase_ = WaitPhase::PARKED;
        if (ready_) {
            queue();
        }
    }

    /// The resume has begun. Does not throw and does not decide the outcome —
    /// that stays with the adapter.
    void begin_resume() noexcept {
        assert(phase_ == WaitPhase::QUEUED || phase_ == WaitPhase::PARKED || phase_ == WaitPhase::IDLE);
        phase_ = WaitPhase::RESUMING;
    }

    /// End the wait and drop every borrow. Destroys no frame and does not claim
    /// that external work is over.
    void finish() noexcept {
        assert(phase_ == WaitPhase::RESUMING && "finishing a wait before resume");
        waiter_ = nullptr;
        phase_ = WaitPhase::FINISHED;
    }

    /// A start that failed before the commit: the adapter has already withdrawn
    /// its event relation, so this only drops the wait's own state.
    void rollback() noexcept {
        assert(phase_ == WaitPhase::ARMING && "rollback outside ARMING");
        waiter_ = nullptr;
        ready_ = false;
        phase_ = WaitPhase::IDLE;
    }

    /// True once this wait has taken the resume entitlement and its waiter has
    /// not resumed yet. Adapters read this instead of a shared scheduled flag.
    [[nodiscard]] bool resume_pending() const noexcept { return phase_ == WaitPhase::QUEUED; }

protected:
    /// Take the resume entitlement. The only place a waiter is queued, so a
    /// duplicate completion or a racing cancellation cannot resume twice. A
    /// claim that arrives while the adapter is still arming is published by the
    /// commit instead, exactly like an early completion: the adapter has not
    /// finished establishing its event relation yet.
    void claim_resume() noexcept {
        if (phase_ == WaitPhase::ARMING) {
            ready_ = true;
        } else if (phase_ == WaitPhase::PARKED) {
            queue();
        }
    }

    /// Queue an armed waiter for resumption.
    void queue() noexcept {
        assert(waiter_ != nullptr && "queueing a wait without a resume target");
        phase_ = WaitPhase::QUEUED;
        schedule_resume(*waiter_);
    }

private:
    ExecutionRecord* waiter_ = nullptr;
    WaitPhase phase_ = WaitPhase::IDLE;
    /// The event completed while the adapter was still arming.
    bool ready_ = false;
};

/// The cancellable half: a WaitCore plus the one subscription that may take the
/// same resume entitlement. Inline and drain waits never construct this, so
/// they carry no subscription storage at all.
class WaitOperation : public WaitCore {
public:
    /// Cancellable adapters own the subscription storage alongside this wait.
    /// The borrowed storage must cover rollback or begin_resume(); no heap
    /// allocation occurs.
    explicit WaitOperation(CancelSubscription& subscription) noexcept : subscription_(&subscription) {}

    WaitOperation(const WaitOperation&) = delete;
    WaitOperation& operator=(const WaitOperation&) = delete;
    WaitOperation(WaitOperation&&) = delete;
    WaitOperation& operator=(WaitOperation&&) = delete;

    /// Arm the cancellation subscription. Only a cancellable event adapter calls
    /// this; the throw for an already-cancelled scope happens before any event
    /// source starts, and leaves nothing armed.
    ///
    /// The scope notifies `this` operation, which then calls the adapter's hook
    /// with the adapter's own context — the subscription never sees the adapter.
    void subscribe_cancel(CancelScope& scope, void* context, CancelAction (*on_cancel)(void*) noexcept) {
        assert(subscription_ != nullptr && "cancellable wait requires subscription storage");
        if (subscription_ == nullptr) {
            std::terminate();
        }
        hook_ = on_cancel;
        subscription_context_ = context;
        SubscriptionAccess::subscribe(scope, *subscription_, this, &WaitOperation::notify_cancelled);
    }

    /// The resume has begun: the subscription is revoked so a cancellation that
    /// arrives now cannot act on a wait that is already being resolved.
    void begin_resume() noexcept {
        if (subscription_ != nullptr) {
            subscription_->disarm();
        }
        WaitCore::begin_resume();
    }

    /// End the wait and drop every borrow, including the subscription.
    void finish() noexcept {
        subscription_ = nullptr;
        WaitCore::finish();
    }

    /// A start that failed before the commit: withdraw the subscription and
    /// drop the wait's own state.
    void rollback() noexcept {
        if (subscription_ != nullptr) {
            subscription_->disarm();
        }
        WaitCore::rollback();
    }

    /// True while this wait is armed on a scope.
    [[nodiscard]] bool subscribed() const noexcept { return subscription_ != nullptr && subscription_->armed(); }

private:
    /// The scope's cancellation, reached through the subscription: the scope has
    /// already popped and unlinked it, so this runs at most once per wait. The
    /// hook does the adapter's own cleanup; the action decides whether the
    /// cancellation also takes the resume entitlement.
    static CancelAction notify_cancelled(void* context) noexcept {
        auto& self = *static_cast<WaitOperation*>(context);
        const CancelAction action =
            self.hook_ != nullptr ? self.hook_(self.subscription_context_) : CancelAction::RESUME;
        if (action == CancelAction::RESUME && !self.resume_pending()) {
            self.claim_resume();
        }
        return action;
    }

    CancelSubscription* subscription_ = nullptr;
    void* subscription_context_ = nullptr;
    CancelAction (*hook_)(void*) noexcept = nullptr;
};

}  // namespace coro::detail

#endif  // YADDNSC_INFRASTRUCTURE_CORO_DETAIL_WAIT_OPERATION_H
