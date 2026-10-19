//
// Coroutine runtime — internal execution record.
//
// The record is the stable owner of one started coroutine frame: it holds the
// frame handle, the loop and effective cancel scope the execution runs under,
// its place in the execution tree, its phase, the borrowed result state and
// the three memberships the loop keeps for it.
//
// It carries state, not policy. Queueing a record, taking and destroying its
// frame and everything that must surround a retirement belong to the loop's
// scheduling and reap transactions; the only notification this type emits is
// the terminal role, from mark_terminal().
//
// Thread safety: none. Loop thread only — activation, scheduling and reaping
// all run there.
//

#ifndef YADDNSC_CORO_DETAIL_EXECUTION_RECORD_H
#define YADDNSC_CORO_DETAIL_EXECUTION_RECORD_H

#include <cassert>
#include <coroutine>
#include <exception>
#include <utility>

#include "coro/detail/forward_list.hpp"
#include "coro/detail/intrusive_list.hpp"
#include "coro/fwd.h"

namespace coro::detail {

class ResultStateBase;

/// Execution phase of a record. The legal transitions form a closed set;
/// "ready", "running" and "suspended" are scheduling state and live in the
/// loop's queues instead of this enum.
enum class ExecutionPhase { PREPARED, ACTIVE, TERMINAL, REAPING, RETIRED };

/// Exclusive owner of one STARTED coroutine frame.
///
/// I1 — a started frame belongs to exactly one record, and only the loop's reap
///      transaction may destroy it. Embedding a record in its host (a run()
///      session, an awaiting frame, a nursery child entry) grants the host
///      storage, never a destruction permission.
/// I2 — the record, its membership hooks and its bound result state are built
///      at their final address before the first resume and are never copied or
///      moved. The host must therefore outlive the whole reap transaction, not
///      just the destroy() call.
///
/// The execution parent and its count of unretired children are a survival
/// relation: they say when the parent may be released, and never schedule it —
/// a child retiring is not a wakeup for its parent.
///
/// Thread safety: none, loop thread only. Every operation is noexcept and
/// allocation-free.
class ExecutionRecord {
public:
    /// Constructs a PREPARED record. Registry/reap targets are this record;
    /// the ready target is bound by adopt(), before the first enqueue.
    ExecutionRecord() noexcept {
        registry_hook_.bind(*this);
        reap_hook_.bind(this);
    }

    /// Called at the host's destructor entry, before any result/payload member
    /// is destroyed. Root storage is checked by its run-session exit boundary.
    void validate_host_lifetime() const noexcept {
        const bool safe = (phase_ == ExecutionPhase::PREPARED || phase_ == ExecutionPhase::RETIRED) && !frame_ &&
                          parent_ == nullptr && unretired_children_ == 0 && !registry_hook_.linked() &&
                          !ready_hook_.linked() && !reap_hook_.linked();
        assert(safe && "execution host destroyed before complete retirement");
        if (!safe) {
            std::terminate();
        }
    }

    ExecutionRecord(const ExecutionRecord&) = delete;
    ExecutionRecord& operator=(const ExecutionRecord&) = delete;
    ExecutionRecord(ExecutionRecord&&) = delete;
    ExecutionRecord& operator=(ExecutionRecord&&) = delete;

    /// The loop that drives this execution; borrowed, never null once bound.
    [[nodiscard]] Loop* loop() const noexcept { return loop_; }

    /// The effective cancel scope of this execution, or null for an execution
    /// that observes no scope. Borrowed; it outlives the record.
    [[nodiscard]] CancelScope* scope() const noexcept { return scope_; }

    /// The borrowed result state, or null before the activation commit binds
    /// it. The slot must outlive the record.
    [[nodiscard]] ResultStateBase* result() const noexcept { return result_; }

    /// The record whose execution this one runs under, or null for a root.
    /// A survival relation, not a wake target.
    [[nodiscard]] ExecutionRecord* parent() const noexcept { return parent_; }

    /// Execution children that have not retired yet. Zero is what lets the
    /// parent finish retiring.
    [[nodiscard]] unsigned unretired_children() const noexcept { return unretired_children_; }

    [[nodiscard]] ExecutionPhase phase() const noexcept { return phase_; }

    /// Whether the loop owns an outstanding ready-queue resumption.
    [[nodiscard]] bool resume_queued() const noexcept { return ready_hook_.linked(); }

    /// Take ownership of a started frame. The activation commit, after every
    /// fallible preparation and before the first enqueue; adopting twice is a
    /// contract violation.
    void adopt(std::coroutine_handle<> frame) noexcept {
        assert(!frame_ && "frame already adopted by this record");
        assert(frame && "cannot adopt a null frame handle");
        frame_ = frame;
        ready_hook_.bind(frame);
    }

    /// Take the frame out of the record, leaving it empty. Only the reap
    /// transaction may call this: once it returns the record owns nothing, and
    /// the caller holds the one handle that may destroy it.
    [[nodiscard]] std::coroutine_handle<> take_frame() noexcept {
        assert(frame_ && "reaping a record that owns no frame");
        if (!frame_) {
            std::terminate();  // destroying a null handle would be undefined behaviour
        }
        return std::exchange(frame_, {});
    }

    /// Bind the loop and the effective cancel scope. Borrowed; both must
    /// outlive the record. Part of the activation commit.
    void bind_context(Loop& loop, CancelScope* scope) noexcept {
        assert(loop_ == nullptr && "execution context rebound");
        loop_ = &loop;
        scope_ = scope;
    }

    /// Bind the result state this execution writes. The slot must be bound
    /// before the first resume and must outlive the record.
    void bind_result(ResultStateBase& result) noexcept {
        assert(result_ == nullptr && "result state rebound");
        result_ = &result;
    }

    /// Assign the fixed completion roles. `on_terminal` runs when the execution
    /// reaches its terminal point (fail-fast bookkeeping: first child defect,
    /// sibling cancellation); `on_retired` runs after the reap transaction
    /// completed the quiet completion (result published, counts committed).
    /// Either may be null. Both are called on the loop thread, may not throw
    /// and may only touch their own bookkeeping.
    ///
    /// The host guarantees that it outlives the whole reap transaction: root
    /// and inline executions own their records locally, and a nursery keeps a
    /// discarded entry alive until after the retirement callback returned.
    /// Fixed roles share an immutable descriptor; only the context is per record.
    template<auto ON_TERMINAL, auto ON_RETIRED>
    void bind_completion(void* context) noexcept {
        static constexpr RecordCompletion COMPLETION{ON_TERMINAL, ON_RETIRED};
        completion_ = &COMPLETION;
        completion_context_ = context;
    }

    /// Count this record as an execution child of `parent`. Called by the
    /// activation commit, so the parent cannot retire while its child runs.
    void attach_to(ExecutionRecord& parent) noexcept {
        assert(parent_ == nullptr && "execution record already attached to a parent");
        parent_ = &parent;
        ++parent.unretired_children_;
    }

    /// Unlink from the execution parent and count one retired child. A root has
    /// no parent, so this is a no-op there.
    void detach_from_parent() noexcept {
        ExecutionRecord* const parent = parent_;
        if (parent == nullptr) {
            return;
        }
        parent_ = nullptr;
        assert(parent->unretired_children_ > 0 && "retired more execution children than were attached");
        --parent->unretired_children_;
    }

    /// PREPARED -> ACTIVE, at the end of the activation commit.
    void mark_active() noexcept {
        require_phase(ExecutionPhase::PREPARED);
        phase_ = ExecutionPhase::ACTIVE;
    }

    /// ACTIVE -> TERMINAL, from the frame's final suspend point. Marks the
    /// phase and runs the terminal notification; no value is moved, no frame is
    /// destroyed and no ordinary consumer is woken.
    void mark_terminal() noexcept {
        require_phase(ExecutionPhase::ACTIVE);
        phase_ = ExecutionPhase::TERMINAL;
        if (completion_->on_terminal != nullptr) {
            completion_->on_terminal(completion_context_, *this);
        }
    }

    /// TERMINAL -> REAPING, as the reap transaction takes the frame.
    ///
    /// The record may not still hold a queued resumption: the ready hook is
    /// unlinked before every resume, so a linked one here means a ready token
    /// still refers to a frame this transaction is about to destroy. The check
    /// is real in Release too — it is the only sentinel left for "frame gone,
    /// token still queued" now that the ready queue carries a borrowed frame
    /// token instead of the record.
    void begin_reap() noexcept {
        require_phase(ExecutionPhase::TERMINAL);
        assert(!ready_hook_.linked() && "reaping a record that is still queued for resumption");
        if (ready_hook_.linked()) {
            std::terminate();  // the queued resume would run a destroyed frame
        }
        phase_ = ExecutionPhase::REAPING;
    }

    /// REAPING -> RETIRED, after the frame is destroyed, every membership is
    /// unlinked and the result is published.
    void mark_retired() noexcept {
        require_phase(ExecutionPhase::REAPING);
        phase_ = ExecutionPhase::RETIRED;
    }

    /// Publish the quiet completion once the reap transaction is through. The
    /// only notification allowed to resume a result consumer or drop a drain
    /// count — the terminal notification may not.
    void notify_retired() noexcept {
        if (completion_->on_retired != nullptr) {
            completion_->on_retired(completion_context_, *this);
        }
    }

private:
    /// The completion roles are one concept: exactly one host owns the record,
    /// and these are the only callbacks it may receive.
    struct RecordCompletion {
        void (*on_terminal)(void*, ExecutionRecord&) noexcept = nullptr;
        void (*on_retired)(void*, ExecutionRecord&) noexcept = nullptr;
    };

    void require_phase(ExecutionPhase expected) const noexcept {
        assert(phase_ == expected && "invalid execution phase transition");
        if (phase_ != expected) {
            std::terminate();
        }
    }

    /// The loop owns every membership link below; it queues and unlinks the
    /// record and is otherwise free to inspect it through this declaration.
    friend class ::coro::Loop;

    std::coroutine_handle<> frame_{};
    Loop* loop_ = nullptr;
    CancelScope* scope_ = nullptr;
    ExecutionRecord* parent_ = nullptr;
    unsigned unretired_children_ = 0;
    ExecutionPhase phase_ = ExecutionPhase::PREPARED;
    ResultStateBase* result_ = nullptr;

    /// Three independent memberships, so a record may sit in more than one.
    /// The registry hook is a ring because an execution leaves it from the
    /// middle. Ready and reap are forward hooks because those queues only
    /// append and pop the front. Membership is the only queue state.
    IntrusiveHook<ExecutionRecord> registry_hook_;
    // Ready dispatch only needs the borrowed frame token, not a record lookup.
    ForwardHook<std::coroutine_handle<>> ready_hook_;
    ForwardHook<ExecutionRecord*> reap_hook_;

    /// Completion roles, assigned by the activation entry that owns them.
    /// Root/inline executions leave the terminal role null; nursery children
    /// set both because the entry outlives the frame's destruction.
    inline static constexpr RecordCompletion NO_COMPLETION{nullptr, nullptr};
    const RecordCompletion* completion_ = &NO_COMPLETION;
    void* completion_context_ = nullptr;
};

}  // namespace coro::detail

#endif  // YADDNSC_CORO_DETAIL_EXECUTION_RECORD_H
