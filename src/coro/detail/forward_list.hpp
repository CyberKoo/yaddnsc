//
// Coroutine runtime — internal forward list.
//
// A singly-linked queue with a head and a tail, plus a borrowed pointer-like
// target in every membership hook. Hosts own their hooks and the list borrows
// them: nothing here allocates, owns a target, or decides when a host may die.
// Structure only — no coroutine, cancellation, result or
// lifecycle meaning belongs in this component.
//
// This is not the doubly-linked ring. That ring stores a predecessor so it can
// detach an arbitrary member. A forward hook has no predecessor, so this list
// cannot. It only appends, pops the front, and steals a whole chain.
//
// Contract:
//   - "Linked" is next_ != this and nothing else. A default-constructed hook
//     points next_ at itself. A hook at the tail stores nullptr, which is still
//     linked: the list, not a self-loop, ends the chain. There is no membership
//     flag and no owner.
//   - Insertion goes through the list. The only way out is pop_front(), which
//     unlinks the head because the list knows that predecessor. There is no
//     unlink of an arbitrary member.
//   - Destroying a linked hook, or a non-empty list, is a contract violation:
//     assert in Debug builds, std::terminate() in Release. Destructors never
//     unlink, so a membership that outlived its node surfaces where it happened.
//   - A consumer that invokes callbacks consumes with pop_front(). A callback
//     may append; the new hook lands at the tail and this walk pops it later.
//
// Thread safety: none. Concurrent mutation from several threads is a caller
// defect; callers that need it must serialize access themselves.
//
#ifndef YADDNSC_INFRASTRUCTURE_CORO_DETAIL_FORWARD_LIST_HPP
#define YADDNSC_INFRASTRUCTURE_CORO_DETAIL_FORWARD_LIST_HPP

#include <cassert>
#include <exception>
#include <type_traits>

namespace coro::detail {

template<typename Target>
class ForwardList;

/// List membership, embedded in its host. Both host and borrowed target must
/// outlive each membership; unlink before either dies. Target is a non-owning
/// pointer-like token: empty
/// by default, truth-testable, and non-throwing to construct and copy. A raw
/// pointer may name an incomplete type; no target is dereferenced here.
template<typename Target>
class ForwardHook {
    static_assert(std::is_trivially_copyable_v<Target> && std::is_nothrow_default_constructible_v<Target>);
    static_assert(noexcept(static_cast<bool>(Target{})));

public:
    /// An unlinked hook with an empty target. bind() must run before
    /// the first insertion.
    ForwardHook() noexcept : next_(this) {}

    /// Requires an unlinked hook: the container never unlinks on destruction, so
    /// a hook that dies inside a list would leave the chain pointing at freed
    /// storage. Debug: assert. Release: std::terminate().
    ~ForwardHook() noexcept {
        assert(!linked() && "destroying a hook that is still linked into a list");
        if (linked()) {
            std::terminate();  // a surviving list would keep a member pointer to freed storage
        }
    }

    ForwardHook(const ForwardHook&) = delete;
    ForwardHook& operator=(const ForwardHook&) = delete;
    ForwardHook(ForwardHook&&) = delete;
    ForwardHook& operator=(ForwardHook&&) = delete;

    /// Assign the borrowed target returned on removal. Idempotent for an unlinked
    /// hook; rebinding a linked hook is a contract violation (Debug: assert,
    /// Release: std::terminate()), because the list being rebound would keep the
    /// old target in its chain.
    void bind(Target target) noexcept {
        assert(!linked() && "rebinding a hook that is still linked into a list");
        if (linked()) {
            std::terminate();  // the chain would keep the previous target
        }
        target_ = target;
    }

    /// True while the hook is a member of some list. Purely the self-pointer
    /// test — there is no separate flag to fall out of sync with the chain.
    /// A tail hook is linked: its next_ is nullptr, and nullptr is not this.
    [[nodiscard]] bool linked() const noexcept { return next_ != this; }

private:
    friend class ForwardList<Target>;

    /// Borrowed target returned by pop_front(); empty until bind().
    Target target_{};
    /// Successor hook, nullptr at the tail, or this when the hook is unlinked.
    ForwardHook* next_ = nullptr;
};

/// A singly-linked queue of hooks. The list owns no host or target: callers
/// embed a ForwardHook, bind its borrowed target, and pop it before either dies.
/// An unlinked hook may retain a stale token; it neither dereferences nor frees it.
/// There is no sentinel. An empty list has a null head and a null tail, and
/// those two stay null together.
///
/// Insertion rejects a hook that is already linked or has an empty target, before
/// touching the chain (Debug: assert, Release: std::terminate()).
template<typename Target>
class ForwardList {
public:
    ForwardList() noexcept = default;

    /// Requires an empty list: the nodes are owned elsewhere and a non-empty
    /// chain here would keep membership they can no longer see (Debug: assert,
    /// Release: std::terminate()). The destructor never clears the list.
    ~ForwardList() noexcept {
        assert(empty() && "destroying a list that still holds nodes");
        if (!empty()) {
            std::terminate();  // the nodes would keep links into released storage
        }
    }

    ForwardList(const ForwardList&) = delete;
    ForwardList& operator=(const ForwardList&) = delete;
    ForwardList(ForwardList&&) = delete;
    ForwardList& operator=(ForwardList&&) = delete;

    /// Link `hook` after the current last node. The hook is not owned by the
    /// list, and its target must outlive the membership.
    void push_back(ForwardHook<Target>& hook) noexcept {
        assert(!hook.linked() && "inserting a hook that is already linked");
        assert(static_cast<bool>(hook.target_) && "inserting a hook with an empty target");
        if (hook.linked() || !hook.target_) {
            std::terminate();  // a partially inserted hook would corrupt the chain
        }
        hook.next_ = nullptr;
        if (tail_ == nullptr) {
            head_ = &hook;
            tail_ = &hook;
            return;
        }
        tail_->next_ = &hook;
        tail_ = &hook;
    }

    /// Detach the first hook and return its target, or an empty target if empty.
    /// The hook is already unlinked, so a callback invoked on its target may
    /// append another hook; that hook lands at the tail and a later pop takes it.
    [[nodiscard]] Target pop_front() noexcept {
        Hook* const first = head_;
        if (first == nullptr) {
            return {};
        }
        head_ = first->next_;
        if (head_ == nullptr) {
            tail_ = nullptr;
        }
        first->next_ = first;
        return first->target_;
    }

    /// Move every node of `source` to the end of this list, in order, leaving
    /// `source` empty. O(1): the chains are joined, no node is rewritten except
    /// the previous tail's successor, and every stolen node stays linked.
    ///
    /// This is how a consumer detaches a whole batch before walking it, so work
    /// appended to `source` while the batch runs lands there instead of inside
    /// the batch. Splicing an empty source is a no-op. Splicing a list into
    /// itself is a contract violation (Debug: assert, Release: std::terminate()).
    void splice_all(ForwardList& source) noexcept {
        assert(this != &source && "splicing a list into itself");
        if (this == &source) {
            std::terminate();  // the chain would be joined onto itself
        }
        if (source.head_ == nullptr) {
            return;
        }
        if (tail_ == nullptr) {
            head_ = source.head_;
        } else {
            tail_->next_ = source.head_;
        }
        tail_ = source.tail_;
        source.head_ = nullptr;
        source.tail_ = nullptr;
    }

    /// True while no hook is linked.
    [[nodiscard]] bool empty() const noexcept { return head_ == nullptr; }

private:
    using Hook = ForwardHook<Target>;

    Hook* head_ = nullptr;
    Hook* tail_ = nullptr;
};

}  // namespace coro::detail

#endif  // YADDNSC_INFRASTRUCTURE_CORO_DETAIL_FORWARD_LIST_HPP
