//
// logging — the asynchronous log pipeline.
//
// Design .cache/coro_redesign.md §2 (thread list) and §6.4: logging is ordinary
// off-loop work. One bounded queue plus one background drain thread — the third
// system thread, after the loop and the offload pool — carries every record off
// the loop, so a log call never blocks on a file/console write. Overflow
// discards the newest record and bumps spdlog's overrun counter; the count is
// reported at shutdown.
//

#ifndef YADDNSC_INFRASTRUCTURE_LOGGING_ASYNC_LOGGING_H
#define YADDNSC_INFRASTRUCTURE_LOGGING_ASYNC_LOGGING_H

namespace logging {

/// Install the process-wide asynchronous pipeline.
///
/// Initializes the spdlog thread pool and replaces the default logger — which
/// both the Logger facade (via SpdlogLogger) and every direct SPDLOG_* call site
/// resolve through — with an async_logger over the same sinks, level and
/// pattern. The `discard_new` overflow policy drops the newest record when the
/// queue is full and increments the pool's overrun counter.
void initialize();

/// Drain the pipeline and report dropped records, then release it.
///
/// Logs a WARN carrying the pool's overrun counter when it is non-zero, then
/// shuts spdlog down, which flushes every logger and joins the drain thread
/// after the queue is emptied — so even a fatal record enqueued at the top-level
/// boundary is written before this returns. Never throws.
void shutdown() noexcept;

}  // namespace logging

#endif  // YADDNSC_INFRASTRUCTURE_LOGGING_ASYNC_LOGGING_H
