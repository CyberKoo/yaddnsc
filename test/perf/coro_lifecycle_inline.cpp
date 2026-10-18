#include <benchmark/benchmark.h>

#include "coro/coro.h"

#include "coro_lifecycle_scenarios.h"

namespace lifecycle_bench {


coro::Task<int> chain_leaf(int value) {
    co_return value;
}

/// `depth` levels of `co_await task`, each level awaiting the next: no group,
/// no handle, just nested inline awaits between neighbouring frames.
coro::Task<int> inline_chain(int depth, int value) {
    if (depth <= 1) {
        co_return co_await chain_leaf(value);
    }
    co_return co_await inline_chain(depth - 1, value);
}

coro::Task<void> inline_chain_bench(int depth, int operations) {
    for (int operation = 0; operation < operations; ++operation) {
        benchmark::DoNotOptimize(co_await inline_chain(depth, operation));
    }
    co_return;
}

}  // namespace lifecycle_bench
