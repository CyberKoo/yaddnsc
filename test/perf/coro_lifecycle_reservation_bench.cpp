#include <cstddef>

#include <benchmark/benchmark.h>

#include "coro/coro.h"
#include "mocks/allocation_fault.h"
#include "support/coro/reservation_pressure.hpp"

namespace {

void bm_reservation_pressure(benchmark::State& state) {
    const int children = static_cast<int>(state.range(0));
    const bool mixed = state.range(1) != 0;
    const bool cancel = state.range(2) != 0;
    std::size_t allocations = 0;
    for (auto _ : state) {
        const test::AllocationCounter counter;
        coro::ManualClock clock;
        coro::Loop loop{clock};
        auto result = coro::run(loop, test::reservation_pressure_batch(children, mixed, cancel));
        allocations += test::AllocationCounter::count();
        if (result.errors != 0) {
            state.SkipWithError("reservation pressure lost results, publication order or cancellation");
            break;
        }
        benchmark::DoNotOptimize(result);
    }
    state.counters["allocations"] =
        benchmark::Counter(static_cast<double>(allocations), benchmark::Counter::kAvgIterations);
    state.SetItemsProcessed(state.iterations() * children);
}

BENCHMARK(bm_reservation_pressure)
    ->Name("BM_reservation_pressure")
    ->ArgsProduct({{4, 16, 64, 256, 1024}, {0, 1}, {0, 1}});

}  // namespace
