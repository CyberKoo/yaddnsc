#ifndef YADDNSC_TEST_SUPPORT_CORO_PAYLOAD_RESIDENCY_HPP
#define YADDNSC_TEST_SUPPORT_CORO_PAYLOAD_RESIDENCY_HPP

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "coro/coro.h"

namespace test {

struct PayloadLedger {
    std::size_t live_frames = 0;
    std::size_t live_payload_bytes = 0;
    std::size_t peak_payload_bytes = 0;
    std::size_t destroyed_payloads = 0;
};

struct ResidencySample {
    coro::Duration loop_elapsed{};
    std::chrono::steady_clock::duration wall_elapsed{};
    std::size_t frames = 0;
    std::size_t payload_bytes = 0;
};

struct ResidencyMeasurement {
    PayloadLedger ledger;
    std::array<ResidencySample, 3> samples{};
    std::size_t invalid_handles_after_exit = 0;
    int errors = 0;
};

struct ResidencySettings {
    int children = 0;
    std::size_t payload_bytes = 0;
    coro::Duration hold{};
    bool consume_before_hold = false;
};

namespace payload_residency {

class Payload {
public:
    Payload(PayloadLedger& ledger, std::size_t bytes)
        : ledger_(&ledger), bytes_(bytes), block_(std::make_unique<std::byte[]>(bytes)) {
        ledger.live_payload_bytes += bytes;
        ledger.peak_payload_bytes = std::max(ledger.peak_payload_bytes, ledger.live_payload_bytes);
    }

    Payload(Payload&& other) noexcept
        : ledger_(std::exchange(other.ledger_, nullptr)), bytes_(other.bytes_), block_(std::move(other.block_)) {}

    Payload(const Payload&) = delete;
    Payload& operator=(const Payload&) = delete;
    Payload& operator=(Payload&&) = delete;

    ~Payload() noexcept {
        if (ledger_ != nullptr) {
            ledger_->live_payload_bytes -= bytes_;
            ++ledger_->destroyed_payloads;
        }
    }

private:
    PayloadLedger* ledger_;
    std::size_t bytes_;
    std::unique_ptr<std::byte[]> block_;
};

class FrameProbe {
public:
    explicit FrameProbe(PayloadLedger& ledger) noexcept : ledger_(&ledger) { ++ledger.live_frames; }

    FrameProbe(FrameProbe&& other) noexcept : ledger_(std::exchange(other.ledger_, nullptr)) {}

    FrameProbe(const FrameProbe&) = delete;
    FrameProbe& operator=(const FrameProbe&) = delete;
    FrameProbe& operator=(FrameProbe&&) = delete;

    ~FrameProbe() noexcept {
        if (ledger_ != nullptr) {
            --ledger_->live_frames;
        }
    }

private:
    PayloadLedger* ledger_;
};

inline coro::Task<Payload> produce(PayloadLedger& ledger, std::size_t bytes, [[maybe_unused]] FrameProbe frame) {
    co_return Payload{ledger, bytes};
}

}  // namespace payload_residency

// Call with a fresh measurement; hold must be positive. Payload bytes count
// owned buffer extents, not array/allocator overhead, entry storage or RSS.
// SystemClock measures actual residence; tests use ManualClock for semantics.
inline coro::Task<void> observe_payload_residency(ResidencySettings settings, ResidencyMeasurement& measurement) {
    auto& ledger = measurement.ledger;
    std::vector<coro::Handle<payload_residency::Payload>> handles;
    handles.reserve(static_cast<std::size_t>(settings.children));
    co_await coro::with_nursery([&](coro::Nursery& nursery) -> coro::Task<void> {
        for (int index = 0; index < settings.children; ++index) {
            handles.push_back(nursery.spawn(
                payload_residency::produce(ledger, settings.payload_bytes, payload_residency::FrameProbe{ledger})));
        }
        while (ledger.live_frames != 0) {
            co_await coro::checkpoint();
        }
        if (settings.consume_before_hold) {
            for (auto& handle : handles) {
                [[maybe_unused]] auto result = co_await handle;
            }
        }
        const auto loop_begin = co_await coro::current_time();
        const auto wall_begin = std::chrono::steady_clock::now();
        const std::array<coro::Duration, 3> offsets{coro::Duration::zero(), settings.hold / 10, settings.hold};
        for (std::size_t index = 0; index < offsets.size(); ++index) {
            if (index != 0) {
                co_await coro::sleep_until(loop_begin + offsets[index]);
            }
            measurement.samples[index] = {(co_await coro::current_time()) - loop_begin,
                                          std::chrono::steady_clock::now() - wall_begin, ledger.live_frames,
                                          ledger.live_payload_bytes};
        }
    });
    for (const auto& handle : handles) {
        measurement.invalid_handles_after_exit += !handle.valid();
    }
    const auto children = static_cast<std::size_t>(settings.children);
    measurement.errors += ledger.live_frames != 0 || ledger.live_payload_bytes != 0;
    measurement.errors += ledger.destroyed_payloads != children;
    measurement.errors += measurement.invalid_handles_after_exit != children;
    const auto expected = settings.consume_before_hold ? 0 : children * settings.payload_bytes;
    for (const auto& sample : measurement.samples) {
        measurement.errors += sample.frames != 0 || sample.payload_bytes != expected;
    }
}

}  // namespace test
#endif  // YADDNSC_TEST_SUPPORT_CORO_PAYLOAD_RESIDENCY_HPP
