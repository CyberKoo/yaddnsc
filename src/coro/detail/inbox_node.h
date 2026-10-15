// Coroutine runtime — cross-thread inbox node.
#ifndef YADDNSC_INFRASTRUCTURE_CORO_DETAIL_INBOX_NODE_H
#define YADDNSC_INFRASTRUCTURE_CORO_DETAIL_INBOX_NODE_H

namespace coro::detail {

/// One cross-thread ingress node for the loop's inbox.
///
/// Any thread links the node onto the loop's lock-free inbox stack and wakes
/// the self-pipe; the loop thread steals the stack and runs each node. A node
/// must not be re-posted before its run() has executed: the stack is a plain
/// Treiber push plus a whole-stack steal, safe only because nodes are never
/// recycled through it. Ownership: a node stays alive from post until run()
/// through a lease its owner holds (e.g. a self-reference); run() releases it.
/// `dispose` runs only for nodes orphaned at loop teardown — it releases the
/// lease without running the node.
struct InboxNode {
    /// Producer-written link, published by the push CAS; loop thread reads it
    /// after the acquiring steal.
    InboxNode* next = nullptr;
    /// Loop-thread dispatch target; must not throw.
    void (*run)(InboxNode&) noexcept = nullptr;
    /// Teardown-only release for orphaned nodes; nullptr means nothing to release.
    void (*dispose)(InboxNode&) noexcept = nullptr;
    /// Trampoline-specific recovery pointer (e.g. the owning offload state).
    void* context = nullptr;
};

}  // namespace coro::detail

#endif  // YADDNSC_INFRASTRUCTURE_CORO_DETAIL_INBOX_NODE_H
