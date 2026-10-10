//
// logging — the asynchronous log pipeline (implementation).
//

#include "async_logging.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <spdlog/async.h>
#include <spdlog/sinks/sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "logging_pattern.h"
#include "support/fmt.hpp"
#include "yaddnsc/util/format.hpp"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not

namespace logging {
namespace {

/// Bounded queue size: large enough for a startup burst (plugin logs, DNS
/// debug) well above the steady-state rate. A full queue must not block the
/// loop, so the overflow policy drops the newest records instead of waiting;
/// their count is reported at shutdown. spdlog keeps the drop count per
/// policy: discard_new accumulates the pool's discard counter, overrun_oldest
/// the overrun counter — shutdown() reads both.
constexpr std::size_t LOG_QUEUE_CAPACITY = 8192;

/// One drain thread. Logging is low-rate and its ordering is irrelevant, so a
/// single worker cannot become a bottleneck; this is the third system thread
/// (loop, offload pool, log drain).
constexpr std::size_t LOG_DRAIN_THREADS = 1;

}  // namespace

void initialize() {
    // Take the sinks the process already logs through (the default logger's
    // coloured stdout sink), so the output target and format stay unchanged.
    const auto existing = spdlog::default_logger();
    const std::vector<spdlog::sink_ptr> sinks = existing->sinks();

    spdlog::init_thread_pool(LOG_QUEUE_CAPACITY, LOG_DRAIN_THREADS);
    auto async = std::make_shared<spdlog::async_logger>(existing->name(), sinks.begin(), sinks.end(),
                                                        spdlog::thread_pool(),
                                                        spdlog::async_overflow_policy::discard_new);
    async->set_level(existing->level());
    async->flush_on(existing->flush_level());

    // set_default_logger also registers it under the same name, so later
    // global spdlog::set_level/set_pattern calls (run -d, config test -q) reach
    // it, and the pattern below applies.
    spdlog::set_default_logger(std::move(async));
    spdlog::set_pattern(std::string{YADDNSC_LOGGING_PATTERN});
}

void shutdown() noexcept {
    try {
        // Read the drop counters through a short-lived handle: keeping a
        // shared_ptr to the pool alive past spdlog::shutdown() would leave the
        // registry's reset as a no-op, and the pool's destructor — which posts
        // the terminate message and joins the drain thread — would never run.
        // discard_new records land on the discard counter, overrun_oldest on
        // the overrun counter; read both so a policy change never silences the
        // warning again.
        std::size_t dropped = 0;
        {
            const auto pool = spdlog::thread_pool();
            dropped = pool != nullptr ? pool->overrun_counter() + pool->discard_counter() : 0;
        }
        if (dropped > 0) {
            // Write the warning straight to the sinks: the queue may still be
            // draining a backlog, and a queued WARN could itself be discarded
            // by the very overflow it reports.
            const auto logger = spdlog::default_logger();
            if (logger != nullptr) {
                const std::string text = fmt::format("Log queue overflowed: {} record(s) discarded", dropped);
                const spdlog::details::log_msg record(spdlog::source_loc{}, logger->name(), spdlog::level::warn,
                                                      std::string_view{text});
                for (const auto& sink : logger->sinks()) {
                    sink->log(record);
                }
            }
        }
        // spdlog::shutdown drops every logger and resets the thread pool; the
        // pool's destructor drains the remaining queue and joins the drain
        // thread, so everything enqueued (including a fatal record) is written
        // before this returns.
        spdlog::shutdown();
        // Leave a synchronous stderr fallback behind: the default logger is
        // null after shutdown, and any late SPDLOG_* call (the drain tail is
        // still unwinding) would otherwise dereference it.
        spdlog::set_default_logger(spdlog::stderr_color_mt("tail"));
        spdlog::set_pattern(std::string{YADDNSC_LOGGING_PATTERN});
    } catch (...) {
        // Shutdown must never terminate the process, and there is nothing left
        // to report to.
    }
}

}  // namespace logging
