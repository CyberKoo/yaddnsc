// Coroutine runtime — implicit context read from a running frame.
#ifndef YADDNSC_INFRASTRUCTURE_CORO_DETAIL_CONTEXT_H
#define YADDNSC_INFRASTRUCTURE_CORO_DETAIL_CONTEXT_H

#include <coroutine>

#include "coro/detail/frame.h"

namespace coro::detail {

/// Implicit context read from the awaiting frame's promise: the loop the task
/// runs on and the innermost scope whose cancellation governs it.
///
/// Runtime-only. The scope reaches application code through
/// coro::current_scope(); the loop never leaves the runtime.
struct Context {
    Loop* loop = nullptr;
    CancelScope* scope = nullptr;
};

/// Reads the awaiting frame's context without suspending.
///
/// Used by the scope combinators and the task group to learn where they run;
/// `co_await GetContext{}` returns the Context and never parks.
class GetContext {
public:
    constexpr bool await_ready() const noexcept { return false; }

    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept {
        PromiseBase& promise = handle.promise();
        context_ = Context{promise.loop, promise.scope};
        return false;  // never actually suspends
    }

    [[nodiscard]] Context await_resume() const noexcept { return context_; }

private:
    Context context_{};
};

}  // namespace coro::detail

#endif  // YADDNSC_INFRASTRUCTURE_CORO_DETAIL_CONTEXT_H