//
// FakeClock — Clock test double with manually advanced time.
//
// wait_until() parks the caller until the fake time reaches the deadline (or
// stop fires), so scheduling tests advance time deterministically instead of
// sleeping.
// =============================================================================

#ifndef YADDNSC_TEST_MOCKS_FAKE_CLOCK_H
#define YADDNSC_TEST_MOCKS_FAKE_CLOCK_H

#include <condition_variable>
#include <mutex>
#include <stop_token>

#include "application/ports/clock.h"

class FakeClock final : public Clock {
public:
    explicit FakeClock(domain::TimePoint start) : now_(start) {}

    [[nodiscard]] domain::TimePoint now() const override {
        std::lock_guard lock(mtx_);
        return now_;
    }

    bool wait_until(domain::TimePoint deadline, const std::stop_token &stop) override {
        std::unique_lock lock(mtx_);
        std::stop_callback cb(stop, [this] { cv_.notify_all(); });
        cv_.wait(lock, [this, deadline, &stop] { return now_ >= deadline || stop.stop_requested(); });
        return !stop.stop_requested();
    }

    /// Advance the fake time and wake every waiter.
    void advance_to(domain::TimePoint t) {
        {
            std::lock_guard lock(mtx_);
            now_ = t;
        }
        cv_.notify_all();
    }

    void advance_by(domain::Duration d) {
        {
            std::lock_guard lock(mtx_);
            now_ += d;
        }
        cv_.notify_all();
    }

private:
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    domain::TimePoint now_;
};

#endif // YADDNSC_TEST_MOCKS_FAKE_CLOCK_H
