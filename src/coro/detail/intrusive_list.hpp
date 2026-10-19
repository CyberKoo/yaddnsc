//
// Coroutine runtime — internal intrusive list.
//
// A circular doubly-linked list with an embedded sentinel, plus a typed
// back-pointer from every membership hook to the node that embeds it. Nodes own
// their hooks and the list borrows them: nothing here allocates, owns a node, or
// decides when a node may die. Structure only — no coroutine, cancellation,
// result or lifecycle meaning belongs in this component.
//
// Contract:
//   - "Linked" is the self-loop and nothing else: a default-constructed hook
//     points next and prev at itself. There is no membership flag and no owner.
//   - Insertion goes through the list; unlinking goes through the hook and is
//     idempotent. There is no remove(list, node).
//   - Destroying a linked hook, or a non-empty list, is a contract violation:
//     assert in Debug builds, std::terminate() in Release. Destructors never
//     unlink, so a membership that outlived its node surfaces where it happened.
//   - Search traverses with next(), which requires a linked hook. A consumer
//     that invokes callbacks consumes with pop_front() instead: a callback may
//     unlink nodes, so a cursor cached across it would dangle.
//
// Thread safety: none. Concurrent mutation from several threads is a caller
// defect; callers that need it must serialize access themselves.
//
#ifndef YADDNSC_CORO_DETAIL_INTRUSIVE_LIST_HPP
#define YADDNSC_CORO_DETAIL_INTRUSIVE_LIST_HPP

#include <cassert>
#include <exception>

namespace coro::detail {

/// List membership, embedded in the node it links. The containing node outlives
/// every list the hook joins, and the hook must be unlinked before that node is
/// destroyed. `Node` may be incomplete: only a pointer to it is stored.
template<typename Node>
class IntrusiveHook {
public:
    /// An unlinked hook that is not bound to a node yet. bind() must run before
    /// the first insertion.
    IntrusiveHook() noexcept : next_(this), prev_(this) {}

    /// Requires an unlinked hook: the container never unlinks on destruction, so
    /// a hook that dies inside a list would leave the ring pointing at freed
    /// storage. Debug: assert. Release: std::terminate().
    ~IntrusiveHook() noexcept {
        assert(!linked() && "destroying a hook that is still linked into a list");
        if (linked()) {
            std::terminate();  // a surviving list would keep a member pointer to freed storage
        }
    }

    IntrusiveHook(const IntrusiveHook&) = delete;
    IntrusiveHook& operator=(const IntrusiveHook&) = delete;
    IntrusiveHook(IntrusiveHook&&) = delete;
    IntrusiveHook& operator=(IntrusiveHook&&) = delete;

    /// Point the hook at the node that embeds it. Idempotent for an unlinked
    /// hook; rebinding a linked hook is a contract violation (Debug: assert,
    /// Release: std::terminate()), because the list being rebound would keep the
    /// old node's address in its ring.
    void bind(Node& node) noexcept {
        assert(!linked() && "rebinding a hook that is still linked into a list");
        if (linked()) {
            std::terminate();  // the ring would keep the previous node's address
        }
        self_ = &node;
    }

    /// The node this hook belongs to. Requires a bound hook: Debug: assert,
    /// Release: std::terminate().
    [[nodiscard]] Node& node() const noexcept {
        assert(self_ != nullptr && "hook was never bound to its node");
        if (self_ == nullptr) {
            std::terminate();
        }
        return *self_;
    }

    /// True while the hook is a member of some list. Purely the self-loop test —
    /// there is no separate flag to fall out of sync with the ring.
    [[nodiscard]] bool linked() const noexcept { return next_ != this; }

    /// The node after this one, or nullptr at the end of the list. Requires a
    /// linked hook: an unlinked hook would answer with itself and turn every
    /// traversal into an endless walk (Debug: assert, Release: std::terminate()).
    /// Search only — a consumer that calls into other code must take nodes with
    /// IntrusiveList::pop_front() instead of caching this pointer.
    [[nodiscard]] Node* next() const noexcept {
        assert(linked() && "next() requires a linked hook");
        if (!linked()) {
            std::terminate();
        }
        return next_->self_;
    }

    /// Detach the hook from whichever list holds it. Idempotent: unlinking an
    /// unlinked hook is a no-op, which is what makes it safe in teardown paths
    /// that run after the node has already left. Never throws.
    void unlink() noexcept {
        if (!linked()) {
            return;
        }
        IntrusiveHook* const before = prev_;
        IntrusiveHook* const after = next_;
        before->next_ = after;
        after->prev_ = before;
        next_ = this;
        prev_ = this;
    }

private:
    template<typename>
    friend class IntrusiveList;

    /// The node embedding this hook; null only before bind() and for the
    /// sentinel, which has no node of its own.
    Node* self_ = nullptr;
    IntrusiveHook* next_ = nullptr;
    IntrusiveHook* prev_ = nullptr;
};

/// A circular doubly-linked list of hooks with an embedded sentinel. The list
/// owns no node: callers embed an IntrusiveHook in the object they want linked,
/// bind it, and unlink it before that object dies. The sentinel carries no node
/// and never leaves the ring, so an empty list is a complete cycle and walking
/// off either end yields nullptr.
///
/// Insertion rejects a hook that is already linked or not bound to a node, before
/// touching the ring (Debug: assert, Release: std::terminate()).
template<typename Node>
class IntrusiveList {
public:
    IntrusiveList() noexcept = default;

    /// Requires an empty list: the nodes are owned elsewhere and a non-empty
    /// ring here would keep membership they can no longer see (Debug: assert,
    /// Release: std::terminate()). The destructor never clears the list.
    ~IntrusiveList() noexcept {
        assert(empty() && "destroying a list that still holds nodes");
        if (!empty()) {
            std::terminate();  // the nodes would keep links into released storage
        }
    }

    IntrusiveList(const IntrusiveList&) = delete;
    IntrusiveList& operator=(const IntrusiveList&) = delete;
    IntrusiveList(IntrusiveList&&) = delete;
    IntrusiveList& operator=(IntrusiveList&&) = delete;

    /// Link `hook` after the current last node. The hook is not owned by the
    /// list, and its bound node must outlive the membership.
    void push_back(IntrusiveHook<Node>& hook) noexcept {
        assert(!hook.linked() && "inserting a hook that is already linked");
        assert(hook.self_ != nullptr && "inserting a hook that was never bound to its node");
        if (hook.linked() || hook.self_ == nullptr) {
            std::terminate();  // a partially inserted hook would corrupt the ring
        }
        Hook* const last = sentinel_.prev_;
        hook.next_ = &sentinel_;
        hook.prev_ = last;
        last->next_ = &hook;
        sentinel_.prev_ = &hook;
    }

    /// Link `hook` before the current first node. Same preconditions as
    /// push_back().
    void push_front(IntrusiveHook<Node>& hook) noexcept {
        assert(!hook.linked() && "inserting a hook that is already linked");
        assert(hook.self_ != nullptr && "inserting a hook that was never bound to its node");
        if (hook.linked() || hook.self_ == nullptr) {
            std::terminate();  // a partially inserted hook would corrupt the ring
        }
        Hook* const first = sentinel_.next_;
        hook.next_ = first;
        hook.prev_ = &sentinel_;
        first->prev_ = &hook;
        sentinel_.next_ = &hook;
    }

    /// Detach the first node and return it, or nullptr when the list is empty.
    /// This is the consumption path: the returned node is already unlinked, so a
    /// callback invoked on it cannot invalidate a cursor this loop is holding.
    [[nodiscard]] Node* pop_front() noexcept {
        Hook* const first = sentinel_.next_;
        if (first == &sentinel_) {
            return nullptr;
        }
        first->unlink();
        return first->self_;
    }

    /// True while no hook is linked.
    [[nodiscard]] bool empty() const noexcept { return sentinel_.next_ == &sentinel_; }

    /// The first node, or nullptr when empty. Search entry point — it does not
    /// unlink, so the caller must unlink before calling out to anything that may
    /// itself consume the list.
    [[nodiscard]] Node* front() const noexcept { return sentinel_.next_->self_; }

private:
    using Hook = IntrusiveHook<Node>;

    /// The ring's end marker: unlinked by definition and holding no node, which
    /// is what turns a walk off the end into a null node.
    Hook sentinel_{};
};

}  // namespace coro::detail

#endif  // YADDNSC_CORO_DETAIL_INTRUSIVE_LIST_HPP
