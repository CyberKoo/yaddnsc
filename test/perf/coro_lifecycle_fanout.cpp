#include <array>

#include <benchmark/benchmark.h>

#include "coro/coro.h"

#include "coro_lifecycle_scenarios.h"

namespace lifecycle_bench {

constexpr int FANOUT_BATCH = 8;

coro::Task<int> fanout_child(int value) {
    co_return value;
}

coro::Task<void> fanout_discard_child(int value) {
    benchmark::DoNotOptimize(value);
    co_return;
}

/// One batch of children spawned, then consumed before the next batch starts:
/// Handle-await, completion-order next<T>(), or fire-and-forget + drain.
coro::Task<void> same_batch_fanout(int mode, int operations) {
    co_await coro::with_nursery([mode, operations](coro::Nursery& group) -> coro::Task<void> {
        std::array<coro::Handle<int>, FANOUT_BATCH> handles;
        for (int operation = 0; operation < operations; ++operation) {
            const int first = operation * FANOUT_BATCH;
            if (mode == CONSUME_DISCARD) {
                // A full discard drain is a nursery boundary, not result consumption.
                co_await coro::with_nursery([first](coro::Nursery& batch) -> coro::Task<void> {
                    for (int child = 0; child < FANOUT_BATCH; ++child) {
                        batch.spawn_discard(fanout_discard_child(first + child));
                    }
                    co_return;
                });
                continue;
            }
            for (int child = 0; child < FANOUT_BATCH; ++child) {
                handles[child] = group.spawn(fanout_child(first + child));
            }
            if (mode == CONSUME_NEXT) {
                for (int child = 0; child < FANOUT_BATCH; ++child) {
                    const auto result = co_await group.next<int>();
                    benchmark::DoNotOptimize(result.has_value() && result->has_value() ? **result : 0);
                }
            } else {
                for (auto& handle : handles) {
                    benchmark::DoNotOptimize(co_await handle);
                }
            }
        }
    });
}

}  // namespace lifecycle_bench
