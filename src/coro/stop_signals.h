#ifndef YADDNSC_CORO_STOP_SIGNALS_H
#define YADDNSC_CORO_STOP_SIGNALS_H

#include <csignal>

#include "coro/task.hpp"
#include "support/util/fd.hpp"

namespace coro {

/// Own SIGINT/SIGTERM handling from startup through worker and log drain.
/// First SIGINT or any SIGTERM latches a stop request and wakes wait(); a second
/// SIGINT immediately exits with status 130. Only one owner may exist at a time.
/// Construct/destroy on the main thread, outside coro::run. Destruction restores
/// the previous handlers after all workers and log threads have been joined.
/// Pipe/handler setup failures throw std::system_error; duplicate ownership is
/// a std::logic_error. The object must outlive every wait and submitted worker.
class StopSignals {
public:
    StopSignals();
    ~StopSignals() noexcept;
    StopSignals(const StopSignals&) = delete;
    StopSignals& operator=(const StopSignals&) = delete;
    StopSignals(StopSignals&&) = delete;
    StopSignals& operator=(StopSignals&&) = delete;

    /// Thread-safe snapshot of the latched stop request.
    [[nodiscard]] bool requested() const noexcept;
    /// Cancellable loop-thread wait; also completes for a signal received before
    /// the wait started. Does not consume/reset the request. Defects propagate.
    [[nodiscard]] coro::Task<void> wait() const;

private:
    Utils::UniqueFd read_;
    Utils::UniqueFd write_;
    struct sigaction previous_int_{};
    struct sigaction previous_term_{};
};

}  // namespace coro

#endif  // YADDNSC_CORO_STOP_SIGNALS_H
