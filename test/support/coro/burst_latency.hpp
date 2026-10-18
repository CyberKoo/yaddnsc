#ifndef YADDNSC_TEST_SUPPORT_CORO_BURST_LATENCY_HPP
#define YADDNSC_TEST_SUPPORT_CORO_BURST_LATENCY_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include <unistd.h>

#include "coro/coro.h"

namespace test {

using ObservationClock = std::chrono::steady_clock;

struct BurstSettings {
    int children = 0;
    int execute_rounds = 0;
    int retire_rounds = 0;
    int read_fd = -1;
    int write_fd = -1;
};

struct BurstMeasurement {
    ObservationClock::time_point event_ready;
    ObservationClock::time_point execution_end;
    ObservationClock::time_point retirement_begin;
    ObservationClock::time_point retirement_end;
    ObservationClock::time_point timer_resumed;
    ObservationClock::time_point io_resumed;
    int armed = 0;
    int executed = 0;
    int retired = 0;
    int timer_saw_retired = 0;
    int io_saw_retired = 0;
    bool io_ok = false;
    std::uint64_t checksum = 1;
};

namespace burst_latency {

// Unsigned work with an observed result, not an optimizer-removable empty loop.
inline std::uint64_t work(std::uint64_t value, int rounds) noexcept {
    for (int index = 0; index < rounds; ++index) {
        value ^= value << 13;
        value ^= value >> 7;
        value ^= value << 17;
    }
    return value;
}

// A coroutine PARAMETER: its active instance dies in frame destruction, not
// when a local scope exits before final_suspend. Moves deactivate the source.
class RetirementProbe {
public:
    RetirementProbe(BurstMeasurement& measurement, int children, int rounds) noexcept
        : measurement_(&measurement), children_(children), rounds_(rounds) {}

    RetirementProbe(RetirementProbe&& other) noexcept
        : measurement_(std::exchange(other.measurement_, nullptr)), children_(other.children_), rounds_(other.rounds_) {
    }

    RetirementProbe(const RetirementProbe&) = delete;
    RetirementProbe& operator=(const RetirementProbe&) = delete;
    RetirementProbe& operator=(RetirementProbe&&) = delete;

    ~RetirementProbe() noexcept {
        if (measurement_ == nullptr) {
            return;
        }
        auto& measurement = *measurement_;
        if (measurement.retired == 0) {
            measurement.retirement_begin = ObservationClock::now();
        }
        measurement.checksum = work(measurement.checksum, rounds_);
        if (++measurement.retired == children_) {
            measurement.retirement_end = ObservationClock::now();
        }
    }

private:
    BurstMeasurement* measurement_;
    int children_;
    int rounds_;
};

inline coro::Task<void> child(coro::Notification& gate, coro::ManualClock& clock, const BurstSettings& settings,
                              BurstMeasurement& measurement, [[maybe_unused]] RetirementProbe retirement) {
    ++measurement.armed;
    co_await gate.wait();
    if (measurement.executed == 0) {
        // The loop already checked timers before entering this detached batch.
        // Make the timer due DURING the batch; measure service delay with the
        // real monotonic clock, independently of simulated timer admission.
        measurement.event_ready = ObservationClock::now();
        clock.advance(std::chrono::milliseconds(1));
        const char byte = 1;
        if (::write(settings.write_fd, &byte, 1) != 1) {
            throw std::runtime_error("burst pipe write failed");
        }
    }
    measurement.checksum = work(measurement.checksum, settings.execute_rounds);
    if (++measurement.executed == settings.children) {
        measurement.execution_end = ObservationClock::now();
    }
}

inline coro::Task<void> timer(BurstMeasurement& measurement) {
    ++measurement.armed;
    co_await coro::sleep_for(std::chrono::milliseconds(1));
    measurement.timer_resumed = ObservationClock::now();
    measurement.timer_saw_retired = measurement.retired;
}

inline coro::Task<void> io(int fd, BurstMeasurement& measurement) {
    ++measurement.armed;
    const auto error = co_await coro::wait_readable(fd);
    measurement.io_resumed = ObservationClock::now();
    measurement.io_saw_retired = measurement.retired;
    char byte = 0;
    measurement.io_ok = !error && ::read(fd, &byte, 1) == 1 && byte == 1;
}

}  // namespace burst_latency

// The pipe must be open and empty. ManualClock controls only which ready batch
// contains the expiry; every measured duration uses the real steady clock.
inline coro::Task<void> observe_ready_burst(BurstSettings settings, coro::ManualClock& clock,
                                            BurstMeasurement& measurement) {
    std::vector<coro::Notification> gates(static_cast<std::size_t>(settings.children));
    co_await coro::with_nursery([&](coro::Nursery& nursery) -> coro::Task<void> {
        nursery.spawn_discard(burst_latency::timer(measurement));
        nursery.spawn_discard(burst_latency::io(settings.read_fd, measurement));
        for (auto& gate : gates) {
            nursery.spawn_discard(burst_latency::child(
                gate, clock, settings, measurement,
                burst_latency::RetirementProbe{measurement, settings.children, settings.retire_rounds}));
        }
        while (measurement.armed != settings.children + 2) {
            co_await coro::checkpoint();
        }
        for (auto& gate : gates) {
            gate.notify();
        }
    });
}

}  // namespace test
#endif  // YADDNSC_TEST_SUPPORT_CORO_BURST_LATENCY_HPP
