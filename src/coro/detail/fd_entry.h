// Coroutine runtime — internal fd registration node.
#ifndef YADDNSC_INFRASTRUCTURE_CORO_DETAIL_FD_ENTRY_H
#define YADDNSC_INFRASTRUCTURE_CORO_DETAIL_FD_ENTRY_H

#include <cstddef>

namespace coro::detail {

/// One fd readiness registration, owned by its awaitable.
///
/// The registering object owns this node and keeps it alive until the wait
/// completes or is removed — the same ownership pattern as the wait operation
/// and TimerNode. The loop holds only the pointer in its poll-order table, so
/// removal is by node identity: O(1), and idempotent through `linked`.
/// Thread safety: loop thread only.
struct FdEntry {
    /// Position in the loop's poll-order table; maintained by the loop.
    std::size_t index = 0;
    int fd = -1;
    short events = 0;
    /// Dispatch target; runs on the loop thread and must not throw.
    void (*fn)(void*, short) noexcept = nullptr;
    void* context = nullptr;
    /// True while the loop's table references this node.
    bool linked = false;
};

}  // namespace coro::detail

#endif  // YADDNSC_INFRASTRUCTURE_CORO_DETAIL_FD_ENTRY_H
