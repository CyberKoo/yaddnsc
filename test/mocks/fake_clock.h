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
        // Registered BEFORE taking the mutex: an already-requested stop runs
        // the callback inline here (not holding mtx_), avoiding self-deadlock.
        // The callback takes mtx_ before notifying, closing the lost-wakeup
        // window between the predicate check and blocking on the cv.
        std::stop_callback cb(stop, [this] {
            { std::lock_guard lock(mtx_); }
            cv_.notify_all();
        });
        std::unique_lock lock(mtx_);
        ++wait_entries_;
        const auto epoch = wake_epoch_;
        cv_.wait(lock, [this, deadline, &stop, epoch] {
            return now_ >= deadline || stop.stop_requested() || wake_epoch_ != epoch;
        });
        return !stop.stop_requested();
    }

    void wake() override {
        {
            std::lock_guard lock(mtx_);
            ++wake_epoch_;
        }
        cv_.notify_all();
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

    /// How often wait_until was entered — tests spin on this to know the
    /// scheduling loop is parked before injecting stimuli.
    [[nodiscard]] unsigned wait_entries() const {
        std::lock_guard lock(mtx_);
        return wait_entries_;
    }

private:
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    domain::TimePoint now_;
    unsigned wake_epoch_ = 0;
    unsigned wait_entries_ = 0;
};

#endif // YADDNSC_TEST_MOCKS_FAKE_CLOCK_H
