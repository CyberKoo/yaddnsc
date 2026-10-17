// Lifecycle cost of the coroutine runtime: deep inline chains, same-batch
// group fan-out, a controlled race consumed through next<T>(), and frame
// destruction overlaid on ready fd/timer events.
//
// This is the baseline the coro v2 lifecycle refactoring is compared against,
// so it is written against the CURRENT API (task_group / TaskGroup / Handle /
// next<T>) and only against APIs that survive the refactoring one-to-one
// (task_group -> with_nursery, next<T> -> next<T>): migrating it is a
// mechanical replacement.
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
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <utility>

#include <benchmark/benchmark.h>
#include <unistd.h>

#include "coro/coro.h"
#include "mocks/allocation_fault.h"
#include "support/util/fd.hpp"

namespace {

constexpr int kFanoutIter = 500;   // batches per BM_same_batch_fanout iteration
constexpr int kFanoutBatch = 8;    // children spawned and consumed in one batch
constexpr int kRaceIter = 100;     // races per BM_controlled_race iteration
constexpr int kOverlayIter = 500;  // overlay batches per BM_destruct_overlay iteration
constexpr int kOverlayFrames = 4;  // payload frames reaped per overlay batch

/// Both overlay families wait this long on the manual clock, so every child of
/// a batch becomes due in the same simulated millisecond and completes in one
/// ready batch together with the fd event.
constexpr coro::Duration kOverlayDelay = std::chrono::milliseconds(1);

constexpr int kConsumeHandle = 0;
constexpr int kConsumeNext = 1;
constexpr int kConsumeDiscard = 2;

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

// ---------------------------------------------------------------------------
//  Deep inline chain — the marginal cost of one awaited level
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
//  Same-batch fan-out — spawn and consume within one group batch
// ---------------------------------------------------------------------------

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
    co_await coro::task_group([mode, operations](coro::TaskGroup& group) -> coro::Task<void> {
        std::array<coro::Handle<int>, kFanoutBatch> handles;
        for (int operation = 0; operation < operations; ++operation) {
            const int first = operation * kFanoutBatch;
            if (mode == kConsumeDiscard) {
                for (int child = 0; child < kFanoutBatch; ++child) {
                    group.spawn_discard(fanout_discard_child(first + child));
                }
                // Drain the batch: join() is the only wait a discard child has.
                co_await group.join();
                continue;
            }
            for (int child = 0; child < kFanoutBatch; ++child) {
                handles[child] = group.spawn(fanout_child(first + child));
            }
            if (mode == kConsumeNext) {
                for (int child = 0; child < kFanoutBatch; ++child) {
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

// ---------------------------------------------------------------------------
//  Controlled race — deterministic staggered completion, consumed by next<T>()
// ---------------------------------------------------------------------------

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
        co_await coro::task_group([children, &out_of_order](coro::TaskGroup& group) -> coro::Task<void> {
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

// ---------------------------------------------------------------------------
//  Destructor overlay — frame teardown sharing batches with fd/timer events
// ---------------------------------------------------------------------------

/// Frame payload with nothing to destroy: the light end of the overlay.
struct LightPayload {
    int value = 0;
};

/// Frame payload owning a heap block whose teardown does measurable work: the
/// destructor touches one byte per cache line on the way out, so the cost is
/// repeatable rather than a free() handing back pages nothing ever faulted.
/// The heavy payload also allocates once per frame, so its allocation count is
/// higher by construction — that difference is part of what is being measured.
struct HeavyPayload {
    static constexpr std::size_t kBytes = 4 * 1024;
    static constexpr std::size_t kStride = 64;  // one byte per cache line

    HeavyPayload() : block(std::make_unique<char[]>(kBytes)) {
        block[0] = 1;
        block[kBytes - 1] = 1;
    }

    HeavyPayload(const HeavyPayload&) = delete;
    HeavyPayload& operator=(const HeavyPayload&) = delete;
    HeavyPayload(HeavyPayload&&) = delete;
    HeavyPayload& operator=(HeavyPayload&&) = delete;

    ~HeavyPayload() {
        std::size_t checksum = 0;
        for (std::size_t offset = 0; offset < kBytes; offset += kStride) {
            checksum += static_cast<unsigned char>(block[offset]);
        }
        benchmark::DoNotOptimize(checksum);
    }

    std::unique_ptr<char[]> block;
};

/// Byte written before the waiter registers, so poll() reports the descriptor
/// ready in the same cycle the staggered timers fire. False when the pipe
/// refused the byte: the fd child is then skipped rather than left waiting for
/// readiness that can never arrive.
bool arm_fd(int fd) noexcept {
    const char byte = 1;
    return ::write(fd, &byte, 1) == 1;
}

/// Consumes the armed byte, so the next batch starts from an empty pipe.
void drain_fd(int fd) noexcept {
    char byte = 0;
    benchmark::DoNotOptimize(::read(fd, &byte, 1));
}

/// Waits for readability, then consumes the byte that made it ready.
coro::Task<void> fd_frame(int fd) {
    benchmark::DoNotOptimize(co_await coro::wait_readable(fd));
    drain_fd(fd);
    co_return;
}

/// A plain deadline wake-up: the batch that reaps the payload frames also
/// carries a timer-driven resumption.
coro::Task<void> timer_frame() {
    co_await coro::sleep_for(kOverlayDelay);
    co_return;
}

/// The payload lives in the frame, so its destructor runs when the loop reaps
/// the frame at the end of the batch that completed it.
template<typename Payload>
coro::Task<void> payload_frame() {
    Payload payload{};
    co_await coro::sleep_for(kOverlayDelay);
    benchmark::DoNotOptimize(&payload);
    co_return;
}

template<typename Payload>
void spawn_payload_frames(coro::TaskGroup& group, int frames) {
    for (int frame = 0; frame < frames; ++frame) {
        group.spawn_discard(payload_frame<Payload>());
    }
}

coro::Task<void> destruct_overlay(bool heavy, int read_fd, int write_fd, int operations) {
    co_await coro::task_group([heavy, read_fd, write_fd, operations](coro::TaskGroup& group) -> coro::Task<void> {
        for (int operation = 0; operation < operations; ++operation) {
            const bool armed = arm_fd(write_fd);
            if (heavy) {
                spawn_payload_frames<HeavyPayload>(group, kOverlayFrames);
            } else {
                spawn_payload_frames<LightPayload>(group, kOverlayFrames);
            }
            if (armed) {
                group.spawn_discard(fd_frame(read_fd));
            }
            group.spawn_discard(timer_frame());
            // Reap this batch before the next one, so every batch pays for the
            // same overlay instead of drifting into the previous one.
            co_await group.join();
        }
    });
}

}  // namespace

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
        coro::run(same_batch_fanout(mode, kFanoutIter));
        allocations += counter.count();
    }
    report_allocations(state, allocations, kFanoutIter);
}

BENCHMARK(BM_same_batch_fanout)->Arg(kConsumeHandle)->Arg(kConsumeNext)->Arg(kConsumeDiscard);

static void BM_controlled_race(benchmark::State& state) {
    const int children = static_cast<int>(state.range(0));
    std::size_t allocations = 0;
    std::size_t out_of_order = 0;
    for (auto _ : state) {
        const test::AllocationCounter counter;
        coro::ManualClock clock;
        coro::Loop loop{clock};
        out_of_order += static_cast<std::size_t>(coro::run(loop, controlled_race(children, kRaceIter)));
        allocations += counter.count();
    }
    // The stagger is the premise of this benchmark: results that arrive in an
    // order other than the spawn order would make its numbers meaningless.
    if (out_of_order != 0) {
        state.SkipWithError("staggered children did not complete in spawn order");
        return;
    }
    benchmark::DoNotOptimize(out_of_order);
    report_allocations(state, allocations, kRaceIter);
}

BENCHMARK(BM_controlled_race)->Arg(2)->Arg(8)->Arg(32);

static void BM_destruct_overlay(benchmark::State& state) {
    const bool heavy = state.range(0) != 0;
    auto [read_end, write_end] = Utils::make_pipe();
    if (!read_end || !write_end) {
        state.SkipWithError("destructor overlay needs a pipe");
        return;
    }
    std::size_t allocations = 0;
    for (auto _ : state) {
        const test::AllocationCounter counter;
        coro::ManualClock clock;
        coro::Loop loop{clock};
        coro::run(loop, destruct_overlay(heavy, read_end.get(), write_end.get(), kOverlayIter));
        allocations += counter.count();
    }
    report_allocations(state, allocations, kOverlayIter);
}

BENCHMARK(BM_destruct_overlay)->Arg(0)->Arg(1);
