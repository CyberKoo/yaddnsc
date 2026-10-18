#include <chrono>
#include <cstddef>
#include <memory>

#include <benchmark/benchmark.h>
#include <unistd.h>

#include "coro/coro.h"

#include "coro_lifecycle_scenarios.h"

namespace lifecycle_bench {

constexpr int OVERLAY_FRAMES = 4;
constexpr coro::Duration OVERLAY_DELAY = std::chrono::milliseconds(1);

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
    static constexpr std::size_t BYTES = 4 * 1024;
    static constexpr std::size_t STRIDE = 64;  // one byte per cache line

    HeavyPayload() : block(std::make_unique<char[]>(BYTES)) {
        block[0] = 1;
        block[BYTES - 1] = 1;
    }

    HeavyPayload(const HeavyPayload&) = delete;
    HeavyPayload& operator=(const HeavyPayload&) = delete;
    HeavyPayload(HeavyPayload&&) = delete;
    HeavyPayload& operator=(HeavyPayload&&) = delete;

    ~HeavyPayload() {
        std::size_t checksum = 0;
        for (std::size_t offset = 0; offset < BYTES; offset += STRIDE) {
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
    co_await coro::sleep_for(OVERLAY_DELAY);
    co_return;
}

/// The payload lives in the frame, so its destructor runs when the loop reaps
/// the frame at the end of the batch that completed it.
template<typename Payload>
coro::Task<void> payload_frame() {
    Payload payload{};
    co_await coro::sleep_for(OVERLAY_DELAY);
    benchmark::DoNotOptimize(&payload);
    co_return;
}

template<typename Payload>
void spawn_payload_frames(coro::Nursery& group, int frames) {
    for (int frame = 0; frame < frames; ++frame) {
        group.spawn_discard(payload_frame<Payload>());
    }
}

coro::Task<void> destruct_overlay(bool heavy, int read_fd, int write_fd, int operations) {
    for (int operation = 0; operation < operations; ++operation) {
        co_await coro::with_nursery([heavy, read_fd, write_fd](coro::Nursery& group) -> coro::Task<void> {
            const bool armed = arm_fd(write_fd);
            if (heavy) {
                spawn_payload_frames<HeavyPayload>(group, OVERLAY_FRAMES);
            } else {
                spawn_payload_frames<LightPayload>(group, OVERLAY_FRAMES);
            }
            if (armed) {
                group.spawn_discard(fd_frame(read_fd));
            }
            group.spawn_discard(timer_frame());
            // Each nursery boundary retires the whole overlay before the next batch.
            co_return;
        });
    }
}

}  // namespace lifecycle_bench
