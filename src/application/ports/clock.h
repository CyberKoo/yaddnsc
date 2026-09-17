//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_APPLICATION_PORTS_CLOCK_H
#define YADDNSC_APPLICATION_PORTS_CLOCK_H

#include <stop_token>

#include "domain/update/time_types.h"

/// Clock — time source port for the scheduling loop.
///
/// The domain (ScheduleQueue) never reads a clock: the scheduler runner pulls
/// `now()` / waits through this port and injects the values. Tests substitute
/// a fake clock and advance time explicitly instead of sleeping.
class Clock {
public:
    virtual ~Clock() = default;

    /// Current time on the scheduling time base.
    [[nodiscard]] virtual domain::TimePoint now() const = 0;

    /// Block until `deadline` is reached.
    /// @return false when `stop` was requested first (caller should exit the
    ///         scheduling loop without popping again).
    virtual bool wait_until(domain::TimePoint deadline, const std::stop_token &stop) = 0;
};

#endif // YADDNSC_APPLICATION_PORTS_CLOCK_H
