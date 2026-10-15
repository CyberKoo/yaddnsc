//
// Coroutine runtime — the "awaited inside coro::run" contract.
//
// Every suspending awaitable parks on the awaiting frame's loop/scope context,
// and only coro::run() (or a context it spawned) binds that context. Awaiting
// from a frame nobody bound is a programming defect, diagnosed by assertion;
// these death tests pin down the diagnosis at the runtime's await entry points:
// co_await Task itself, plus the two awaitables that used to throw
// std::logic_error instead. Assertions compile out under NDEBUG, so the file
// is empty in a Release test build.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; these tests use EXPECT_* only.
//

#include <chrono>
#include <exception>
#include <string>

#include <coroutine>

#include <gtest/gtest.h>

#include "coro/coro.h"
#include "coro/detail/frame.h"

#ifndef NDEBUG

namespace {

// A coroutine whose promise carries the runtime frame header but whose context
// nobody ever binds — the state of a frame started without coro::run().
// Resuming it by hand runs the body with loop == nullptr.
struct UnboundTask {
    struct promise_type : coro::detail::PromiseBase {
        UnboundTask get_return_object() noexcept {
            return UnboundTask{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        // The coroutine promise protocol requires instance methods here.
        // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
        std::suspend_always initial_suspend() noexcept { return {}; }

        // The coroutine promise protocol requires instance methods here.
        // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
        std::suspend_always final_suspend() noexcept { return {}; }

        void return_void() noexcept {}

        // The coroutine promise protocol requires instance methods here.
        // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
        void unhandled_exception() { std::terminate(); }
    };

    std::coroutine_handle<promise_type> handle;

    ~UnboundTask() { handle.destroy(); }
};

}  // namespace

TEST(ContextContract, task_await_OutsideRun_AssertionDiagnosed) {
    EXPECT_DEATH(
        {
            UnboundTask frame = []() -> UnboundTask { co_await []() -> coro::Task<void> { co_return; }(); }();
            frame.handle.resume();
        },
        "must be awaited inside coro::run");
}

TEST(ContextContract, sleep_for_OutsideRun_AssertionDiagnosed) {
    EXPECT_DEATH(
        {
            UnboundTask frame = []() -> UnboundTask { co_await coro::sleep_for(std::chrono::milliseconds{1}); }();
            frame.handle.resume();
        },
        "must be awaited inside coro::run");
}

TEST(ContextContract, wait_readable_OutsideRun_AssertionDiagnosed) {
    EXPECT_DEATH(
        {
            // fd 0 passes the fd_ < 0 guard, so only the missing loop can fire.
            UnboundTask frame = []() -> UnboundTask { co_await coro::wait_readable(0); }();
            frame.handle.resume();
        },
        "must be awaited inside coro::run");
}

#endif  // NDEBUG
