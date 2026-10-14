//
// Coroutine runtime — the injected trace sink is the only way loop diagnostics
// leave the module; the runtime holds no logging backend.
//

#include <algorithm>
#include <atomic>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <coroutine>

#include <gtest/gtest.h>

#include "coro/coro.h"

namespace {

struct TraceRecord {
    std::string message;
    std::string file;
    int line = 0;
};

struct TraceRecorder {
    std::vector<TraceRecord> records;
};

void record_trace(void* context, std::string_view message, const std::source_location& where) noexcept {
    auto* recorder = static_cast<TraceRecorder*>(context);
    recorder->records.push_back(TraceRecord{std::string(message), where.file_name(), static_cast<int>(where.line())});
}

coro::Task<int> forty_two() {
    co_return 42;
}

}  // namespace

TEST(TraceSink, InstalledSink_ReceivesEventsWithCallSite) {
    // The recorder is declared before the loop so it outlives it: the loop's
    // own teardown (self-pipe removal) also reports through the sink.
    TraceRecorder recorder;
    coro::Loop loop;
    loop.set_trace_sink(&record_trace, &recorder);

    EXPECT_EQ(coro::run(loop, forty_two()), 42);

    const auto started = std::ranges::find_if(recorder.records, [](const TraceRecord& record) {
        return record.message.find("run() starting") != std::string::npos;
    });
    ASSERT_NE(started, recorder.records.end());
    EXPECT_NE(started->file.find("loop.cpp"), std::string::npos);
    EXPECT_GT(started->line, 0);

    // Every record carries the location of its own trace site, not the sink's.
    EXPECT_TRUE(std::ranges::all_of(recorder.records, [](const TraceRecord& record) {
        return record.file.find("loop.cpp") != std::string::npos && record.line > 0;
    }));
}

TEST(TraceSink, CrossThreadPosts_SinkAlwaysRunsOnLoopThread) {
    struct State {
        std::thread::id owner = std::this_thread::get_id();
        std::atomic<bool> wrong_thread{false};
    } state;

    coro::Loop loop;
    loop.set_trace_sink(
        [](void* context, std::string_view, const std::source_location&) noexcept {
            auto& observed = *static_cast<State*>(context);
            if (std::this_thread::get_id() != observed.owner) {
                observed.wrong_thread.store(true);
            }
        },
        &state);
    auto root = [&]() -> coro::Task<void> {
        co_await coro::task_group([](coro::TaskGroup& group) -> coro::Task<void> {
            for (int i = 0; i < 20; ++i) {
                group.spawn(coro::offload([] {}));
            }
            co_return;
        });
    };
    coro::run(loop, root());
    EXPECT_FALSE(state.wrong_thread.load());
}

TEST(TraceSink, ClearedSink_StaysSilent) {
    TraceRecorder recorder;
    coro::Loop loop;
    loop.set_trace_sink(&record_trace, &recorder);
    loop.set_trace_sink(nullptr, nullptr);

    EXPECT_EQ(coro::run(loop, forty_two()), 42);
    EXPECT_TRUE(recorder.records.empty());
}
