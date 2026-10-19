// Lifecycle cost of the coroutine runtime: deep inline chains, same-batch
// nursery fan-out, a controlled race consumed through next<T>(), and frame
// destruction overlaid on ready fd/timer events.
//
// Uses the nursery API. Historical comparisons must match both workload and
// lifecycle semantics; discard and destructor-overlay boundaries changed when
// nursery drain replaced the old group join, so they are not identical baselines.
//
// Every benchmark follows coro_offload_bench's pattern: coro::run() takes a
// *fresh* loop per call (self-pipe + sigaction), so N operations run INSIDE one
// task and the loop's fixed setup/teardown is amortised the way ns/op already
// amortises it. `allocations` additionally divides by the inner operation
// count, so it is per operation rather than per benchmark iteration. The Time
// column stays per benchmark iteration, as in coro_offload_bench.
//
// The two benchmarks whose timing must not depend on wall time
// (BM_controlled_race, BM_destruct_overlay) run on a coro::ManualClock, which
// the loop advances to the next deadline instead of sleeping: their timing is
// deterministic and costs no real milliseconds.
//
// Every task body contains a co_* keyword, even when there is nothing to
// await: a function returning Task whose body has no co_await/co_return is
// NOT a coroutine ([dcl.fct.def.coroutine]) but an ordinary function that
// falls off its end without returning the Task — UB that GCC 15 turns into an
// infinite loop in Release (UBSan reports the missing return in Debug).
#include <cstddef>

#include <benchmark/benchmark.h>

#include "coro/coro.h"
#include "coro/detail/pipe.h"
#include "mocks/allocation_fault.h"

#include "coro_lifecycle_scenarios.h"

namespace {

using namespace lifecycle_bench;

/// `allocations` is the total over every benchmark iteration. kAvgIterations
/// makes Google Benchmark divide by the iteration count, so dividing by the
/// inner operation count here leaves allocations per operation — the same
/// amortisation ns/op gets from the inner loop.
void report_allocations(benchmark::State& state, std::size_t allocations, int operations) {
    state.counters["allocations"] =
        benchmark::Counter(static_cast<double>(allocations) / operations, benchmark::Counter::kAvgIterations);
    state.SetItemsProcessed(state.iterations() * operations);
}

/// Inner operations per benchmark iteration, scaled down as the chain deepens:
/// a 512-level chain costs orders of magnitude more than a single level, and a
/// fixed count would let the deep case own the whole benchmark runtime.
int chain_operations(int depth) {
    if (depth <= 8) {
        return 2000;
    }
    if (depth <= 64) {
        return 250;
    }
    return 50;
}

static void BM_inline_chain(benchmark::State& state) {
    const int depth = static_cast<int>(state.range(0));
    const int operations = chain_operations(depth);
    std::size_t allocations = 0;
    for (auto _ : state) {
        const test::AllocationCounter counter;
        coro::run(inline_chain_bench(depth, operations));
        allocations += counter.count();
    }
    report_allocations(state, allocations, operations);
}

BENCHMARK(BM_inline_chain)->Arg(1)->Arg(8)->Arg(64)->Arg(512);

static void BM_same_batch_fanout(benchmark::State& state) {
    const int mode = static_cast<int>(state.range(0));
    std::size_t allocations = 0;
    for (auto _ : state) {
        const test::AllocationCounter counter;
        coro::run(same_batch_fanout(mode, FANOUT_ITERATIONS));
        allocations += counter.count();
    }
    report_allocations(state, allocations, FANOUT_ITERATIONS);
}

BENCHMARK(BM_same_batch_fanout)->Arg(CONSUME_HANDLE)->Arg(CONSUME_NEXT)->Arg(CONSUME_DISCARD);

static void BM_controlled_race(benchmark::State& state) {
    const int children = static_cast<int>(state.range(0));
    std::size_t allocations = 0;
    std::size_t out_of_order = 0;
    for (auto _ : state) {
        const test::AllocationCounter counter;
        coro::ManualClock clock;
        coro::Loop loop{clock};
        out_of_order += static_cast<std::size_t>(coro::run(loop, controlled_race(children, RACE_ITERATIONS)));
        allocations += counter.count();
    }
    // The stagger is the premise of this benchmark: results that arrive in an
    // order other than the spawn order would make its numbers meaningless.
    if (out_of_order != 0) {
        state.SkipWithError("staggered children did not complete in spawn order");
        return;
    }
    benchmark::DoNotOptimize(out_of_order);
    report_allocations(state, allocations, RACE_ITERATIONS);
}

BENCHMARK(BM_controlled_race)->Arg(2)->Arg(8)->Arg(32);

static void BM_destruct_overlay(benchmark::State& state) {
    const bool heavy = state.range(0) != 0;
    auto [read_end, write_end] = coro::detail::make_pipe();
    if (!read_end || !write_end) {
        state.SkipWithError("destructor overlay needs a pipe");
        return;
    }
    std::size_t allocations = 0;
    for (auto _ : state) {
        const test::AllocationCounter counter;
        coro::ManualClock clock;
        coro::Loop loop{clock};
        coro::run(loop, destruct_overlay(heavy, read_end.get(), write_end.get(), OVERLAY_ITERATIONS));
        allocations += counter.count();
    }
    report_allocations(state, allocations, OVERLAY_ITERATIONS);
}

BENCHMARK(BM_destruct_overlay)->Arg(0)->Arg(1);

static void BM_runtime_layout(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(sizeof(coro::detail::PromiseBase));
    }
    state.counters["promise_base_bytes"] = sizeof(coro::detail::PromiseBase);
    state.counters["promise_int_bytes"] = sizeof(coro::detail::TaskPromise<int>);
    state.counters["record_bytes"] = sizeof(coro::detail::ExecutionRecord);
    state.counters["inline_awaiter_int_bytes"] = sizeof(coro::detail::TaskAwaiter<int>);
    state.counters["entry_int_bytes"] = sizeof(coro::detail::ChildEntry<int>);
    state.counters["wait_bytes"] = sizeof(coro::detail::WaitOperation);
    state.counters["plain_wait_bytes"] = sizeof(coro::detail::WaitCore);
}

BENCHMARK(BM_runtime_layout);

}  // namespace
