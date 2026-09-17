//
// Created by Kotarou on 2026/9/17.
//

#include "steady_clock.h"

domain::TimePoint SteadyClock::now() const {
    return std::chrono::steady_clock::now();
}

bool SteadyClock::wait_until(domain::TimePoint deadline, const std::stop_token &stop) {
    if (stop.stop_requested()) {
        return false;
    }

    std::unique_lock lock(mtx_);
    // Wakes every waiter as soon as stop fires; destroyed when the wait ends.
    const std::stop_callback cb(stop, [this] { cv_.notify_all(); });
    const auto epoch = wake_epoch_;
    cv_.wait_until(lock, deadline, [this, &stop, epoch] {
        return stop.stop_requested() || wake_epoch_ != epoch;
    });
    return !stop.stop_requested();
}

void SteadyClock::wake() {
    {
        std::lock_guard lock(mtx_);
        ++wake_epoch_;
    }
    cv_.notify_all();
}
