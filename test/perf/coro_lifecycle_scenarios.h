#ifndef YADDNSC_TEST_PERF_CORO_LIFECYCLE_SCENARIOS_H
#define YADDNSC_TEST_PERF_CORO_LIFECYCLE_SCENARIOS_H

#include "coro/task.hpp"

namespace lifecycle_bench {

inline constexpr int FANOUT_ITERATIONS = 500;
inline constexpr int RACE_ITERATIONS = 100;
inline constexpr int OVERLAY_ITERATIONS = 500;
inline constexpr int CONSUME_HANDLE = 0;
inline constexpr int CONSUME_NEXT = 1;
inline constexpr int CONSUME_DISCARD = 2;

coro::Task<void> inline_chain_bench(int depth, int operations);
coro::Task<void> same_batch_fanout(int mode, int operations);
coro::Task<int> controlled_race(int children, int races);
coro::Task<void> destruct_overlay(bool heavy, int read_fd, int write_fd, int operations);

}  // namespace lifecycle_bench
#endif  // YADDNSC_TEST_PERF_CORO_LIFECYCLE_SCENARIOS_H
