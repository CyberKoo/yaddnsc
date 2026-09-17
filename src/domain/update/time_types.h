//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_DOMAIN_UPDATE_TIME_TYPES_H
#define YADDNSC_DOMAIN_UPDATE_TIME_TYPES_H

#include <chrono>

/// Time base shared by the scheduling domain and the Clock port.
///
/// Deadlines are steady-clock based (immune to wall-clock adjustments). The
/// domain never reads the clock itself: every value is injected by the caller,
/// so tests advance a fake time base instead of sleeping.
namespace domain {

using TimePoint = std::chrono::steady_clock::time_point;
using Duration = std::chrono::steady_clock::duration;

} // namespace domain

#endif // YADDNSC_DOMAIN_UPDATE_TIME_TYPES_H
