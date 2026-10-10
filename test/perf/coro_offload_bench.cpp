// Temporary probe: marginal cost of each way to write coroutine work.
// coro::run() takes a *fresh* loop per call, so every benchmark loops N times
// INSIDE one task — that isolates the marginal cost of the awaited operation
// from the loop's fixed setup/teardown (self-pipe + sigaction).
#include <benchmark/benchmark.h>

#include <cstdint>
#include "infrastructure/coro/coro.h"

namespace {
constexpr int kIter = 2000;
}

static void BM_offload_roundtrip(benchmark::State& state) {
    for (auto _ : state) {
        coro::run([]() -> coro::Task<void> {
            for (int i = 0; i < kIter; ++i) {
                benchmark::DoNotOptimize(co_await coro::offload([] { return 1; }));
            }
        }());
    }
}
BENCHMARK(BM_offload_roundtrip);

static void BM_checkpoint(benchmark::State& state) {
    for (auto _ : state) {
        coro::run([]() -> coro::Task<void> {
            for (int i = 0; i < kIter; ++i) {
                co_await coro::checkpoint();
            }
        }());
    }
}
BENCHMARK(BM_checkpoint);

// Cost of one inline computation — the alternative to offloading it.
static void BM_inline_compute(benchmark::State& state) {
    for (auto _ : state) {
        coro::run([]() -> coro::Task<void> {
            for (int i = 0; i < kIter; ++i) {
                uint64_t acc = 0;
                for (uint64_t j = 0; j < 200; ++j) {  // a few hundred ns of arithmetic
                    acc += j * 2654435761ULL;
                }
                benchmark::DoNotOptimize(acc);
            }
        }());
    }
}
BENCHMARK(BM_inline_compute);

// Bare task body with no await at all — the floor.
static void BM_no_await(benchmark::State& state) {
    for (auto _ : state) {
        coro::run([]() -> coro::Task<void> {
            for (int i = 0; i < kIter; ++i) {
                benchmark::DoNotOptimize(i);
            }
        }());
    }
}
BENCHMARK(BM_no_await);
