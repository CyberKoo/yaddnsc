//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_INFRASTRUCTURE_TIME_STEADY_CLOCK_H
#define YADDNSC_INFRASTRUCTURE_TIME_STEADY_CLOCK_H

#include <condition_variable>
#include <mutex>

#include "application/ports/clock.h"
#include "domain/update/time_types.h"

/// SteadyClock — Clock adapter over std::chrono::steady_clock.
///
/// wait_until() parks the caller on a condition variable until the deadline;
/// requesting stop wakes every waiter immediately (a stop callback notifies
/// the condition variable, same wakeup path the legacy Scheduler used).
class SteadyClock final : public Clock {
public:
    [[nodiscard]] domain::TimePoint now() const override;

    /// Block until `deadline`; returns false when stop was requested first.
    bool wait_until(domain::TimePoint deadline, const std::stop_token& stop) override;

    /// Wake every waiter early (retry requests can move deadlines sooner).
    void wake() override;

private:
    std::mutex mtx_;
    std::condition_variable cv_;
    // Bumped by wake(); wait_until returns early when it changes so callers
    // re-evaluate their deadline (a plain notify would be swallowed by the
    // wait predicate).
    unsigned wake_epoch_ = 0;
};

#endif  // YADDNSC_INFRASTRUCTURE_TIME_STEADY_CLOCK_H
