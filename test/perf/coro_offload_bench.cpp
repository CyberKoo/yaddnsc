// Marginal cost of each way to write coroutine work.
// coro::run() takes a *fresh* loop per call, so every benchmark loops N times
// INSIDE one task — that isolates the marginal cost of the awaited operation
// from the loop's fixed setup/teardown (self-pipe + sigaction).
//
// Every task body contains a co_* keyword, even when there is nothing to
// await: a function returning Task whose body has no co_await/co_return is
// NOT a coroutine ([dcl.fct.def.coroutine]) but an ordinary function that
// falls off its end without returning the Task — UB that GCC 15 turns into
// an infinite loop in Release (UBSan reports the missing return in Debug).
#include <cstdint>

#include <benchmark/benchmark.h>

#include "coro/coro.h"
#include "domain/network/inet_address.h"
#include "infrastructure/ip_source/iface_util.h"

namespace {

constexpr int kIter = 2000;

coro::Task<void> offload_roundtrips() {
    for (int i = 0; i < kIter; ++i) {
        benchmark::DoNotOptimize(co_await coro::offload([] { return 1; }));
    }
}

coro::Task<int> child_value() {
    co_return 1;
}

coro::Task<void> group_consumption(bool next) {
    co_await coro::with_nursery([next](coro::Nursery& group) -> coro::Task<void> {
        for (int i = 0; i < kIter; ++i) {
            auto handle = group.spawn(child_value());
            if (next) {
                benchmark::DoNotOptimize((co_await group.next<int>())->value());
            } else {
                benchmark::DoNotOptimize(co_await handle);
            }
        }
    });
}

coro::Task<void> checkpoints() {
    for (int i = 0; i < kIter; ++i) {
        co_await coro::checkpoint();
    }
}

// Cost of one inline computation — the alternative to offloading it.
coro::Task<void> inline_compute() {
    for (int i = 0; i < kIter; ++i) {
        uint64_t acc = 0;
        for (uint64_t j = 0; j < 200; ++j) {  // a few hundred ns of arithmetic
            acc += j * 2654435761ULL;
        }
        benchmark::DoNotOptimize(acc);
    }
    co_return;
}

// Bare task body with no await at all — the floor.
coro::Task<void> no_await() {
    for (int i = 0; i < kIter; ++i) {
        benchmark::DoNotOptimize(i);
    }
    co_return;
}

}  // namespace

static void BM_offload_roundtrip(benchmark::State& state) {
    for (auto _ : state) {
        coro::run(offload_roundtrips());
    }
}

BENCHMARK(BM_offload_roundtrip);

static void BM_group_consumption(benchmark::State& state) {
    for (auto _ : state) {
        coro::run(group_consumption(state.range(0) != 0));
    }
    state.SetItemsProcessed(state.iterations() * kIter);
}

BENCHMARK(BM_group_consumption)->Arg(0)->Arg(1);

static void BM_checkpoint(benchmark::State& state) {
    for (auto _ : state) {
        coro::run(checkpoints());
    }
}

BENCHMARK(BM_checkpoint);

static void BM_inline_compute(benchmark::State& state) {
    for (auto _ : state) {
        coro::run(inline_compute());
    }
}

BENCHMARK(BM_inline_compute);

static void BM_no_await(benchmark::State& state) {
    for (auto _ : state) {
        coro::run(no_await());
    }
}

BENCHMARK(BM_no_await);

// Compare the old synchronous metadata path with the owned offload path.
// This measures lookup/round-trip cost, not a latency bound for getifaddrs.
static void BM_metadata_snapshot(benchmark::State& state) {
    for (auto _ : state) {
        auto query = [&]() -> coro::Task<void> {
            for (int i = 0; i < kIter; ++i) {
                if (state.range(0) == 0) {
                    benchmark::DoNotOptimize(ipsource::get_all());
                } else {
                    benchmark::DoNotOptimize(co_await coro::offload([] { return ipsource::get_all(); }));
                }
            }
        };
        coro::run(query());
    }
    state.SetItemsProcessed(state.iterations() * kIter);
}

BENCHMARK(BM_metadata_snapshot)->Arg(0)->Arg(1)->UseRealTime();
