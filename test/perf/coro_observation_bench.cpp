#include <chrono>
#include <cstddef>
#include <exception>
#include <string>

#include <benchmark/benchmark.h>

#include "coro/coro.h"
#include "coro/detail/pipe.h"
#include "mocks/allocation_fault.h"
#include "support/coro/burst_latency.hpp"
#include "support/coro/payload_residency.hpp"

namespace {

double nanoseconds(test::ObservationClock::duration duration) {
    return std::chrono::duration<double, std::nano>(duration).count();
}

void report_average(benchmark::State& state, const std::string& name, double total) {
    state.counters[name] = benchmark::Counter(total, benchmark::Counter::kAvgIterations);
}

// Each repetition is ONE burst, making JSON repetition rows actual latency
// samples, not averages of several bursts. Use --benchmark_repetitions for the
// distribution; no production trace hooks or per-frame timing probes are used.
void bm_ready_burst(benchmark::State& state) {
    const int children = static_cast<int>(state.range(0));
    const int mode = static_cast<int>(state.range(1));
    auto [read_end, write_end] = coro::detail::make_pipe();
    if (!read_end || !write_end) {
        state.SkipWithError("burst observation needs a pipe");
        return;
    }
    double timer_ns = 0, io_ns = 0, execution_ns = 0, retirement_ns = 0, gap_ns = 0, allocations = 0;
    for (auto _ : state) {
        test::BurstMeasurement measurement;
        const test::BurstSettings settings{children, mode == 1 ? 256 : 0, mode == 2 ? 256 : 0, read_end.get(),
                                           write_end.get()};
        try {
            const test::AllocationCounter counter;
            coro::ManualClock clock;
            coro::Loop loop{clock};
            coro::run(loop, test::observe_ready_burst(settings, clock, measurement));
            allocations += static_cast<double>(counter.count());
        } catch (const std::exception& error) {
            state.SkipWithError(error.what());
            return;
        }
        if (!measurement.io_ok || measurement.executed != children || measurement.retired != children ||
            measurement.timer_saw_retired != children || measurement.io_saw_retired != children ||
            measurement.execution_end < measurement.event_ready ||
            measurement.retirement_begin < measurement.execution_end ||
            measurement.retirement_end < measurement.retirement_begin ||
            measurement.timer_resumed < measurement.retirement_end ||
            measurement.io_resumed < measurement.retirement_end) {
            state.SkipWithError("burst observation violated ready/reap/service ordering");
            return;
        }
        timer_ns += nanoseconds(measurement.timer_resumed - measurement.event_ready);
        io_ns += nanoseconds(measurement.io_resumed - measurement.event_ready);
        execution_ns += nanoseconds(measurement.execution_end - measurement.event_ready);
        retirement_ns += nanoseconds(measurement.retirement_end - measurement.retirement_begin);
        gap_ns += nanoseconds(measurement.retirement_begin - measurement.execution_end);
        benchmark::DoNotOptimize(measurement.checksum);
    }
    report_average(state, "timer_service_ns", timer_ns);
    report_average(state, "io_service_ns", io_ns);
    report_average(state, "execution_span_ns", execution_ns);
    report_average(state, "retirement_span_ns", retirement_ns);
    report_average(state, "execution_to_retirement_ns", gap_ns);
    report_average(state, "allocations", allocations);
}

BENCHMARK(bm_ready_burst)->Name("BM_ready_burst")->ArgsProduct({{8, 64, 512, 4096}, {0, 1, 2}})->Iterations(1);

// Actual SystemClock residence, not simulated elapsed time. The semantic unit
// test uses a ManualClock to exercise the same workload without sleeping.
void bm_payload_residency(benchmark::State& state) {
    const test::ResidencySettings settings{static_cast<int>(state.range(0)), static_cast<std::size_t>(state.range(1)),
                                           std::chrono::milliseconds(state.range(2)), state.range(3) != 0};
    for (auto _ : state) {
        test::ResidencyMeasurement measurement;
        std::size_t allocations = 0;
        try {
            const test::AllocationCounter counter;
            coro::Loop loop;
            coro::run(loop, test::observe_payload_residency(settings, measurement));
            allocations = counter.count();
        } catch (const std::exception& error) {
            state.SkipWithError(error.what());
            return;
        }
        if (measurement.errors != 0) {
            state.SkipWithError("payload residence or cleanup did not match its ownership");
            return;
        }
        state.counters["allocations"] = static_cast<double>(allocations);
        state.counters["peak_payload_bytes"] = static_cast<double>(measurement.ledger.peak_payload_bytes);
        state.counters["payload_bytes_after_exit"] = static_cast<double>(measurement.ledger.live_payload_bytes);
        state.counters["invalid_handles_after_exit"] = static_cast<double>(measurement.invalid_handles_after_exit);
        for (std::size_t index = 0; index < measurement.samples.size(); ++index) {
            const auto prefix = "sample_" + std::to_string(index);
            const auto& sample = measurement.samples[index];
            state.counters[prefix + "_payload_bytes"] = static_cast<double>(sample.payload_bytes);
            state.counters[prefix + "_live_frames"] = static_cast<double>(sample.frames);
            state.counters[prefix + "_wall_ms"] =
                std::chrono::duration<double, std::milli>(sample.wall_elapsed).count();
        }
    }
}

BENCHMARK(bm_payload_residency)
    ->Name("BM_payload_residency")
    ->ArgsProduct({{64, 1024}, {256, 4096}, {1000}, {0, 1}})
    ->Args({1024, 4096, 60000, 0})
    ->Args({1024, 4096, 60000, 1})
    ->Iterations(1)
    ->UseRealTime();

}  // namespace
