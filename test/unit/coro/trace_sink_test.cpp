//
// Coroutine runtime — the injected trace sink is the only way loop diagnostics
// leave the module; the runtime holds no logging backend.
//

#include <algorithm>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

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

TEST(TraceSink, ClearedSink_StaysSilent) {
    TraceRecorder recorder;
    coro::Loop loop;
    loop.set_trace_sink(&record_trace, &recorder);
    loop.set_trace_sink(nullptr, nullptr);

    EXPECT_EQ(coro::run(loop, forty_two()), 42);
    EXPECT_TRUE(recorder.records.empty());
}
