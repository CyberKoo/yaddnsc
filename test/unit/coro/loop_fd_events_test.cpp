//
// Coroutine runtime — fd readiness events and loop teardown.
//
// Two zero-coverage surfaces of the reworked loop:
//
//   - describe_poll_events(), the trace formatter behind every fd trace site.
//     It is reachable only when a trace sink is installed AND an fd is
//     registered, so the existing trace tests never reached it.
//   - the orphan-inbox drain in ~Loop(). A node posted after the loop has
//     stopped polling never runs; it is released through its dispose hook.
//     Nothing posted that late ever exercised it.
//
// The event arms are driven with real descriptors — a pipe whose peer closed
// (POLLHUP), a descriptor closed after registration (POLLNVAL) — and the trace
// assertions check the rendered event names, which is the diagnostic output
// itself rather than a side effect.
//
// Nothing here needs a network, a clock or a worker pool.
//

#include <algorithm>
#include <atomic>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <coroutine>
#include <unistd.h>

#include <gtest/gtest.h>

#include "coro/coro.h"
#include "support/util/fd.hpp"

namespace {

/// Collected trace lines, so a test can assert on the rendered event names.
struct TraceRecorder {
    std::vector<std::string> lines;
};

void record_trace(void* context, std::string_view message, const std::source_location& /*where*/) noexcept {
    static_cast<TraceRecorder*>(context)->lines.emplace_back(message);
}

[[nodiscard]] bool any_line_contains(const TraceRecorder& recorder, std::string_view needle) {
    return std::any_of(recorder.lines.begin(), recorder.lines.end(),
                       [needle](const std::string& line) { return line.find(needle) != std::string::npos; });
}

/// A trace sink is a diagnostic channel, not a behaviour switch: install one
/// where the event name itself is under test.
///
/// The recorder is declared BEFORE the loop on purpose: members are destroyed
/// in reverse declaration order, so this makes ~Loop() run first. The loop
/// still emits traces while tearing down (closing the self-pipe calls
/// remove_fd), and a sink installed on a dead recorder would be a
/// heap-use-after-free.
class TracingLoop {
private:
    TraceRecorder recorder_;
    coro::Loop loop_;

public:
    TracingLoop() { loop_.set_trace_sink(&record_trace, &recorder_); }

    [[nodiscard]] coro::Loop& loop() noexcept { return loop_; }
    [[nodiscard]] const TraceRecorder& recorder() const noexcept { return recorder_; }
};

}  // namespace

// ---------------------------------------------------------------------------
// fd readiness events, observed through the trace
// ---------------------------------------------------------------------------

TEST(FdWaitEvents, RegisteringADescriptor_TracesTheRequestedEvents) {
    TracingLoop tracing;
    auto [read_end, write_end] = Utils::make_pipe();
    ASSERT_TRUE(read_end);
    ASSERT_TRUE(write_end);
    // A readable descriptor, so the wait completes instead of parking the loop.
    const char byte = 'x';
    ASSERT_EQ(::write(write_end.get(), &byte, 1), 1);

    auto root = [fd = read_end.get()]() -> coro::Task<void> {
        const std::error_code ec = co_await coro::wait_readable(fd);
        EXPECT_FALSE(ec);
    };
    EXPECT_NO_THROW(coro::run(tracing.loop(), root()));

    EXPECT_TRUE(any_line_contains(tracing.recorder(), "Registered fd"));
    EXPECT_TRUE(any_line_contains(tracing.recorder(), "POLLIN"))
        << "rendered events: " << tracing.recorder().lines.size() << " trace line(s)";
}

TEST(FdWaitEvents, PeerHangup_RendersPollhupAndStaysReadiness) {
    TracingLoop tracing;
    auto [read_end, write_end] = Utils::make_pipe();
    ASSERT_TRUE(read_end);
    write_end.reset();  // peer gone: reads report POLLHUP

    auto root = [fd = read_end.get()]() -> coro::Task<void> {
        // A hangup is readiness, not an invalid descriptor: the wait resolves.
        EXPECT_FALSE(co_await coro::wait_readable(fd));
    };
    EXPECT_NO_THROW(coro::run(tracing.loop(), root()));

    EXPECT_TRUE(any_line_contains(tracing.recorder(), "POLLHUP"))
        << "hangup must reach the trace renderer as POLLHUP";
    // The ready-path trace is the one that formats the *reported* events.
    EXPECT_TRUE(any_line_contains(tracing.recorder(), "ready:"));
}

TEST(FdWaitEvents, DescriptorClosedAfterRegistration_ThrowsAndRendersPollnval) {
    TracingLoop tracing;
    auto [read_end, write_end] = Utils::make_pipe();
    ASSERT_TRUE(read_end);
    ASSERT_TRUE(write_end);
    const int fd = read_end.get();

    // Deterministic ordering: the close runs as a posted closure on the loop
    // thread, which processes its inbox after this frame has registered the
    // descriptor. The next poll therefore reports POLLNVAL for it — no sleep,
    // no race on a timer.
    auto root = [&tracing, fd]() -> coro::Task<void> {
        tracing.loop().post([fd] { ::close(fd); });
        [[maybe_unused]] const std::error_code ec = co_await coro::wait_readable(fd);
    };
    EXPECT_THROW(coro::run(tracing.loop(), root()), std::invalid_argument);

    EXPECT_TRUE(any_line_contains(tracing.recorder(), "POLLNVAL"))
        << "a descriptor closed under the loop must render as POLLNVAL";
}

// ---------------------------------------------------------------------------
// ~Loop orphan drain
// ---------------------------------------------------------------------------

TEST(LoopTeardown, PostedAfterShutdown_IsReleasedWithoutRunning) {
    std::atomic<bool> ran{false};
    {
        coro::Loop loop;
        EXPECT_EQ(coro::run(loop, []() -> coro::Task<int> { co_return 42; }()), 42);
        // The loop has stopped polling. This node can never run: ~Loop must
        // release it through its dispose hook instead of leaving it in the
        // inbox, and must not invoke the callback.
        loop.post([&ran] { ran.store(true, std::memory_order_release); });
        EXPECT_FALSE(ran.load(std::memory_order_acquire));
    }
    EXPECT_FALSE(ran.load(std::memory_order_acquire))
        << "a node orphaned at teardown must be disposed, never run";
}

TEST(LoopTeardown, PostedBeforeShutdown_StillRuns) {
    // The counterpart to the case above: the same post() call, but the loop
    // polls once more, so the node runs through its normal path. Without this
    // the teardown test would pass even if post() stopped reaching the inbox.
    std::atomic<bool> ran{false};
    coro::Loop loop;
    loop.post([&ran] { ran.store(true, std::memory_order_release); });
    EXPECT_EQ(coro::run(loop, []() -> coro::Task<int> { co_return 42; }()), 42);
    EXPECT_TRUE(ran.load(std::memory_order_acquire));
}
