//
// Coroutine runtime — SerialLane (composition-layer tool, not a runtime
// primitive).
//
// One lane per plugin instance: create/update/destroy submitted to the same
// lane execute in submission order, because the lane keeps a loop-side FIFO and
// a single "busy" flag. Under abandon semantics an abandoned update still runs
// on the worker, and a later destroy waits behind it — order is a property of
// the data structure, not of timing luck. Queued jobs always run: the lane's
// job is ordering, not admission.
//
// Built on offload (design §6.2): a lane owns no threads of its own, it just
// serializes what reaches the loop's pool.
//

#ifndef YADDNSC_CORO_SERIAL_LANE_HPP
#define YADDNSC_CORO_SERIAL_LANE_HPP

#include <cassert>
#include <cstddef>
#include <deque>
#include <exception>
#include <memory>
#include <system_error>
#include <type_traits>
#include <utility>

#include <coroutine>

#include <expected>

#include "infrastructure/coro/cancel_scope.h"
#include "infrastructure/coro/fwd.h"
#include "infrastructure/coro/loop.h"
#include "infrastructure/coro/offload.hpp"
#include "infrastructure/coro/result_box.hpp"
#include "infrastructure/coro/task.hpp"

namespace coro {

namespace detail {

/// Type-erased unit of work queued on a lane.
///
/// Ownership: held by std::shared_ptr; the lane's pending_ queue and the
/// worker's completion hop each hold a share, so an abandoned await cannot
/// destroy a job that is still running or queued.
/// Thread safety: `run()` executes on a pool worker; everything else
/// (waiter/awaiting/cancelled/completed) is loop-thread state.
struct LaneJob {
    virtual ~LaneJob() = default;
    virtual void run() = 0;

    PromiseBase* waiter = nullptr;
    bool awaiting = false;
    bool cancelled = false;
    bool completed = false;
    std::exception_ptr error;
    WaitNode node{};
};

template<typename F>
struct LaneJobImpl final : LaneJob {
    using R = std::invoke_result_t<F>;

    std::shared_ptr<F> fn;
    ResultBox<R> result;

    /// Runs on a worker thread; captures a defect into the job instead of
    /// letting it escape the pool.
    void run() override {
        try {
            result.invoke(*fn);
        } catch (...) {
            error = std::current_exception();
        }
    }
};

template<typename F>
struct LaneAwaitable;

}  // namespace detail

/// Serializes work submitted to one plugin instance onto the offload pool.
///
/// Ownership: borrows the Loop (taken from the first await); the lane must
/// outlive every submit() in flight because completion hops call back into it.
/// Thread safety: loop thread only for submit and every internal method; jobs
/// themselves run on pool workers and must not touch lane state.
class SerialLane {
public:
    SerialLane() = default;
    SerialLane(const SerialLane&) = delete;
    SerialLane& operator=(const SerialLane&) = delete;
    ~SerialLane() noexcept = default;

    /// Queue work on this lane in submission order.
    ///
    /// The job is submitted on the awaiting thread but always dispatched after
    /// the previously queued job has finished, even if that await was
    /// abandoned. Cancellation: abandon — the await yields
    /// `unexpected(operation_canceled)` while the job keeps its place in the
    /// lane. Failure: allocation may throw; a defect thrown by `fn` is
    /// rethrown at the await point.
    template<typename F>
    [[nodiscard]] auto submit(F&& fn) -> Task<std::expected<std::invoke_result_t<std::decay_t<F>>, std::errc>> {
        using Fn = std::decay_t<F>;
        co_return co_await detail::LaneAwaitable<Fn>{this, std::make_shared<Fn>(std::forward<F>(fn))};
    }

private:
    template<typename F>
    friend struct detail::LaneAwaitable;

    void enqueue(std::shared_ptr<detail::LaneJob> job, Loop& loop) {
        loop_ = &loop;
        pending_.push_back(std::move(job));
        if (!busy_) {
            dispatch_next();
        }
    }

    void dispatch_next() {
        if (pending_.empty()) {
            busy_ = false;
            return;
        }
        busy_ = true;
        std::shared_ptr<detail::LaneJob> job = pending_.front();
        pending_.pop_front();
        Loop* loop = loop_;
        SerialLane* self = this;
        loop->offload_pool().detach_task([job, loop, self] {
            job->run();
            loop->post([job, self] { self->on_finished(job); });
        });
    }

    void on_finished(const std::shared_ptr<detail::LaneJob>& job) {
        busy_ = false;
        job->completed = true;
        if (job->awaiting && !job->cancelled && job->waiter != nullptr && !job->node.scheduled) {
            job->node.scheduled = true;
            if (job->node.linked && job->node.scope != nullptr) {
                job->node.scope->remove_waiter(job->node);
            }
            wake(*job->waiter);
        }
        dispatch_next();
    }

    std::deque<std::shared_ptr<detail::LaneJob>> pending_;
    Loop* loop_ = nullptr;
    bool busy_ = false;
};

namespace detail {

/// Awaits one lane job.
///
/// Ownership: holds a shared_ptr to the job, which the lane queue also keeps;
/// the awaiter's frame may die on abandon without disturbing the job.
/// Thread safety: both awaits run on the loop thread.
template<typename F>
struct LaneAwaitable {
    using R = std::invoke_result_t<F>;

    SerialLane* lane = nullptr;
    std::shared_ptr<F> fn;
    std::shared_ptr<LaneJobImpl<F>> job = std::make_shared<LaneJobImpl<F>>();

    bool await_ready() const noexcept { return false; }

    /// Registers with the lane and the awaiting scope, then parks.
    /// Allocates, so it may throw.
    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        PromiseBase& promise = handle.promise();
        Loop* loop = promise.loop;
        CancelScope* scope = promise.scope;
        assert(loop != nullptr && "SerialLane::submit must be awaited inside coro::run");
        if (scope != nullptr && scope->cancelled()) {
            job->cancelled = true;
            return false;
        }
        job->fn = fn;
        job->waiter = &promise;
        job->awaiting = true;
        if (scope != nullptr) {
            job->node.waiter = &promise;
            job->node.cancelled_flag = &job->cancelled;
            job->node.owner = job.get();
            scope->add_waiter(job->node);
        }
        lane->enqueue(job, *loop);
        return true;
    }

    /// Yields the job's value, `operation_canceled` when abandoned, or
    /// rethrows the defect the job captured.
    std::expected<R, std::errc> await_resume() {
        if (job->node.linked && job->node.scope != nullptr) {
            job->node.scope->remove_waiter(job->node);
        }
        if (job->cancelled) {
            return std::unexpected(std::errc::operation_canceled);
        }
        if (job->error) {
            std::rethrow_exception(job->error);
        }
        if constexpr (std::is_void_v<R>) {
            job->result.take();
            return {};
        } else {
            return job->result.take();
        }
    }
};

}  // namespace detail

}  // namespace coro

#endif  // YADDNSC_CORO_SERIAL_LANE_HPP
