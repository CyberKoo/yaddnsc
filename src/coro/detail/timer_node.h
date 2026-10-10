// Coroutine runtime — internal timer-heap node.
#ifndef YADDNSC_INFRASTRUCTURE_CORO_DETAIL_TIMER_NODE_H
#define YADDNSC_INFRASTRUCTURE_CORO_DETAIL_TIMER_NODE_H

#include <cstddef>
#include <cstdint>

#include "coro/time.h"

namespace coro::detail {

/// Entry in the loop's timer heap. The waiting frame owns the node, so it must
/// be removed before the frame is destroyed. `action` runs on the loop thread
/// and must not throw.
struct TimerNode {
    TimePoint deadline{};
    std::uint64_t sequence = 0;
    std::size_t heap_index = 0;
    bool in_heap = false;
    void (*action)(void*) noexcept = nullptr;
    void* context = nullptr;
};

}  // namespace coro::detail

#endif  // YADDNSC_INFRASTRUCTURE_CORO_DETAIL_TIMER_NODE_H
