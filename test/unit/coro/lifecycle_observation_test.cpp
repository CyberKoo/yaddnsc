#include <chrono>
#include <cstddef>

#include <gtest/gtest.h>

#include "coro/coro.h"
#include "support/coro/burst_latency.hpp"
#include "support/coro/payload_residency.hpp"
#include "support/util/fd.hpp"

TEST(CoroObservation, InBurstTimerAndIoWaitForWholeBatchExecutionAndRetirement) {
    for (const int children : {1, 64, 512}) {
        for (const int mode : {0, 1, 2}) {
            SCOPED_TRACE(::testing::Message() << "children=" << children << " mode=" << mode);
            auto [read_end, write_end] = Utils::make_pipe();
            ASSERT_TRUE(read_end);
            ASSERT_TRUE(write_end);
            coro::ManualClock clock;
            coro::Loop loop{clock};
            test::BurstMeasurement measurement;
            const test::BurstSettings settings{children, mode == 1 ? 256 : 0, mode == 2 ? 256 : 0, read_end.get(),
                                               write_end.get()};
            coro::run(loop, test::observe_ready_burst(settings, clock, measurement));
            EXPECT_TRUE(loop.session_complete());
            EXPECT_TRUE(measurement.io_ok);
            EXPECT_EQ(measurement.executed, children);
            EXPECT_EQ(measurement.retired, children);
            EXPECT_EQ(measurement.timer_saw_retired, children);
            EXPECT_EQ(measurement.io_saw_retired, children);
            EXPECT_LE(measurement.event_ready, measurement.execution_end);
            EXPECT_LE(measurement.execution_end, measurement.retirement_begin);
            EXPECT_LE(measurement.retirement_begin, measurement.retirement_end);
            EXPECT_LE(measurement.retirement_end, measurement.timer_resumed);
            EXPECT_LE(measurement.retirement_end, measurement.io_resumed);
        }
    }
}

TEST(CoroObservation, UnconsumedPayloadsOutliveFramesButNotConsumptionOrNurseryExit) {
    for (const int children : {1, 64, 256}) {
        for (const bool consume : {false, true}) {
            SCOPED_TRACE(::testing::Message() << "children=" << children << " consume=" << consume);
            coro::ManualClock clock;
            coro::Loop loop{clock};
            test::ResidencyMeasurement measurement;
            const test::ResidencySettings settings{children, 4096, std::chrono::hours(1), consume};
            coro::run(loop, test::observe_payload_residency(settings, measurement));
            EXPECT_EQ(measurement.errors, 0);
            EXPECT_EQ(measurement.ledger.peak_payload_bytes, static_cast<std::size_t>(children) * 4096);
            EXPECT_EQ(measurement.ledger.live_payload_bytes, 0U);
            EXPECT_EQ(measurement.ledger.live_frames, 0U);
            EXPECT_EQ(measurement.invalid_handles_after_exit, static_cast<std::size_t>(children));
            EXPECT_GE(measurement.samples.back().loop_elapsed, settings.hold);
            for (const auto& sample : measurement.samples) {
                EXPECT_EQ(sample.frames, 0U);
                EXPECT_EQ(sample.payload_bytes, consume ? 0U : static_cast<std::size_t>(children) * 4096);
            }
        }
    }
}
