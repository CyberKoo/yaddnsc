//
// logging — the asynchronous log pipeline.
//
// Design .cache/coro_redesign.md §2 (thread list) and §6.4: logging is ordinary
// off-loop work. One bounded queue plus one background drain thread — the third
// system thread, after the loop and the offload pool — carries every record off
// the loop, so a log call never blocks on a file/console write. Overflow
// discards the newest record and bumps spdlog's discard counter; the count is
// reported at shutdown.
//

#ifndef YADDNSC_INFRASTRUCTURE_LOGGING_ASYNC_LOGGING_H
#define YADDNSC_INFRASTRUCTURE_LOGGING_ASYNC_LOGGING_H

namespace logging {

/// Install the process-wide asynchronous pipeline.
///
/// Initializes the spdlog thread pool and replaces the default logger — which
/// both the app::LoggerPort facade (via SpdlogLogger) and every direct SPDLOG_* call site
/// resolve through — with an async_logger over the same sinks, level and
/// pattern. The `discard_new` overflow policy drops the newest record when the
/// queue is full and increments the pool's discard counter.
void initialize();

/// Drain the pipeline and report dropped records, then release it.
///
/// Writes a WARN carrying the pool's combined drop count (discard plus overrun
/// counters) directly to the sinks when it is non-zero — a queued WARN could
/// itself be discarded while a backlog is still draining — then shuts spdlog
/// down, which flushes every logger and joins the drain thread after the queue
/// is emptied — so even a fatal record enqueued at the top-level boundary is
/// written before this returns. Never throws.
void shutdown() noexcept;

}  // namespace logging

#endif  // YADDNSC_INFRASTRUCTURE_LOGGING_ASYNC_LOGGING_H
