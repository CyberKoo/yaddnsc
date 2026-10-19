// Coroutine runtime — the offload admission node.
//
// One cancellable FIFO admission, owned by the awaiter's shared state and moved
// to the pool on admission; clearing `job` breaks its state lease. The node is a
// plain, non-template leaf so the loop can keep its admission queue, its worker
// capacity accounting and its submission path independent of the awaiter that
// fills a node in.
//
// The completion contract below is the loop's whole view of an awaiter: an opaque
// context plus one callback that reports a failed submission. It names neither
// the awaitable nor its result cell, so loop.cpp never needs the offload
// awaitable's definition.
#ifndef YADDNSC_CORO_DETAIL_OFFLOAD_ADMISSION_H
#define YADDNSC_CORO_DETAIL_OFFLOAD_ADMISSION_H

#include <exception>
#include <functional>

namespace coro::detail {

struct OffloadAdmission {
    OffloadAdmission* next = nullptr;
    OffloadAdmission* prev = nullptr;
    std::function<void()> job;
    /// Neutral completion contract: on a failed submission the loop calls this
    /// with `owner` and the exception, leaving the awaiter's own bookkeeping to
    /// it. Never null once the node carries a job.
    void (*failed)(void*, std::exception_ptr) noexcept = nullptr;
    void* owner = nullptr;
    bool linked = false;
    bool submitted = false;
};

}  // namespace coro::detail

#endif  // YADDNSC_CORO_DETAIL_OFFLOAD_ADMISSION_H
