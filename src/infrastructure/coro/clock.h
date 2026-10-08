//
// Coroutine runtime — loop clock.
//
// The clock is a property of the loop, never an API parameter. Production code
// uses the system clock; tests construct a manual clock and let the loop jump it
// forward to the next timer deadline, which makes timer behaviour deterministic
// without any time plumbing in the public API.
//

#ifndef YADDNSC_CORO_CLOCK_H
#define YADDNSC_CORO_CLOCK_H

#include <chrono>

namespace coro {

using TimePoint = std::chrono::steady_clock::time_point;
using Duration = std::chrono::steady_clock::duration;

/// Time source owned by a Loop.
///
/// Thread safety: the loop reads the clock on its own thread only; an
/// implementation need not be synchronized.
/// Lifetime: a loop borrows the clock, so the clock must outlive the loop.
class Clock {
public:
    Clock() = default;
    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;
    virtual ~Clock() = default;

    /// Current time. Must not throw (the loop calls it from noexcept paths).
    [[nodiscard]] virtual TimePoint now() const noexcept = 0;

    /// A manual clock never blocks: the loop advances it to the next deadline
    /// instead of sleeping, and polls file descriptors without waiting.
    [[nodiscard]] virtual bool manual() const noexcept { return false; }

    /// Advance a manual clock to `tp`. No-op for real clocks.
    virtual void jump_to([[maybe_unused]] TimePoint tp) noexcept {}
};

/// Wall-clock time source (std::chrono::steady_clock).
class SystemClock final : public Clock {
public:
    [[nodiscard]] TimePoint now() const noexcept override { return std::chrono::steady_clock::now(); }
};

/// Deterministic test clock; only the loop advances it.
///
/// `jump_to()` (driven by the loop when it would otherwise block) and
/// `advance()` (driven by a test) are the only ways time moves, so a test can
/// assert exactly how far simulated time travelled.
class ManualClock final : public Clock {
public:
    explicit ManualClock(TimePoint start = TimePoint{}) noexcept : now_(start) {}

    [[nodiscard]] TimePoint now() const noexcept override { return now_; }

    [[nodiscard]] bool manual() const noexcept override { return true; }

    void jump_to(TimePoint tp) noexcept override {
        if (tp > now_) {
            now_ = tp;
        }
    }

    /// Explicitly advance the clock (tests may drive it directly).
    void advance(Duration delta) noexcept { now_ += delta; }

private:
    TimePoint now_{};
};

}  // namespace coro

#endif  // YADDNSC_CORO_CLOCK_H
