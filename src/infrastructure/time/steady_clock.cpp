//
// Created by Kotarou on 2026/9/17.
//

#include "steady_clock.h"

#include <chrono>
#include <stop_token>

domain::TimePoint SteadyClock::now() const {
    return std::chrono::steady_clock::now();
}

bool SteadyClock::wait_until(domain::TimePoint deadline, const std::stop_token& stop) {
    if (stop.stop_requested()) {
        return false;
    }

    // Registered BEFORE taking the mutex: an already-requested stop runs the
    // callback inline here (not holding mtx_), avoiding self-deadlock. The
    // callback takes mtx_ before notifying, closing the lost-wakeup window
    // between the predicate check and blocking — a missed stop here could
    // park an empty queue at TimePoint::max().
    const std::stop_callback cb(stop, [this] {
        {
            std::lock_guard lock(mtx_);
        }
        cv_.notify_all();
    });
    std::unique_lock lock(mtx_);
    const auto epoch = wake_epoch_;
    cv_.wait_until(lock, deadline, [this, &stop, epoch] { return stop.stop_requested() || wake_epoch_ != epoch; });
    return !stop.stop_requested();
}

void SteadyClock::wake() {
    {
        std::lock_guard lock(mtx_);
        ++wake_epoch_;
    }
    cv_.notify_all();
}
