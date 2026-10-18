#include <chrono>

#include <benchmark/benchmark.h>

#include "coro/coro.h"

#include "coro_lifecycle_scenarios.h"

namespace lifecycle_bench {


/// Completes after `rank` milliseconds of simulated time. The manual clock
/// advances deadline by deadline, so completion order is the spawn order: a
/// controlled stand-in for the DNS race, minus the wall-clock jitter.
coro::Task<int> staggered_child(int rank) {
    co_await coro::sleep_for(std::chrono::milliseconds(rank + 1));
    co_return rank;
}

/// Out-of-order results would mean the stagger stopped being deterministic,
/// which is the whole premise of this benchmark; the count is returned so the
/// benchmark can skip itself instead of asserting inside the measured loop.
coro::Task<int> controlled_race(int children, int races) {
    int out_of_order = 0;
    for (int race = 0; race < races; ++race) {
        co_await coro::with_nursery([children, &out_of_order](coro::Nursery& group) -> coro::Task<void> {
            for (int rank = 0; rank < children; ++rank) {
                group.spawn(staggered_child(rank));
            }
            for (int consumed = 0; consumed < children; ++consumed) {
                const auto result = co_await group.next<int>();
                const int rank = result.has_value() && result->has_value() ? **result : -1;
                out_of_order += rank != consumed;
            }
        });
    }
    co_return out_of_order;
}

}  // namespace lifecycle_bench
