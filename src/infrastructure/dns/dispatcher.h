//
// dns — resolver dispatcher: fallback, shuffle and concurrent strategies.
//
// The concurrent race is the redesign's task_group showcase: at most
// MAX_CONCURRENT_RESOLVERS queries fly at once, each transient failure launches
// the next backend at once, the winner cancels its siblings through a child
// cancel scope, and scope exit still joins and reaps every child (design §3.2).
// Retry backoff is a cancellable sleep, not a thread.
//

#ifndef YADDNSC_INFRASTRUCTURE_DNS_DISPATCHER_H
#define YADDNSC_INFRASTRUCTURE_DNS_DISPATCHER_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <expected>
#include <cstddef>

#include "domain/error/dns_error_info.h"
#include "coro/task.hpp"

namespace domain {
enum class RecordKind;
}  // namespace domain

namespace dns {
class Resolver;

/// How a dispatcher uses its backends.
enum class Strategy {
    FALLBACK,    ///< Try backends in configured order until one answers.
    SHUFFLE,     ///< Same, in a random order per query.
    CONCURRENT,  ///< Race up to three backends at a time; the first answer wins.
};

/// Composes resolver backends behind one query operation.
///
/// Ownership: owns the backends.
/// Failure: expected<records, DnsErrorInfo>. A definitive error (PARSE/CONFIG)
/// recorded by any backend is never replaced by a later transient one, and
/// NXDOMAIN stops the search. A retryable failure in single-backend mode is
/// retried with a cancellable backoff; in multi-backend mode the redundancy of
/// several backends replaces retries.
/// Cancellation: `coro::Cancelled` as a value; every await is a checkpoint.
/// Thread safety: not thread-safe as an object; concurrent queries on one
/// dispatcher are not supported (each backend owns its own session state).
class Dispatcher {
public:
    explicit Dispatcher(std::vector<std::unique_ptr<Resolver>> resolvers, Strategy strategy = Strategy::CONCURRENT);

    ~Dispatcher();

    Dispatcher(const Dispatcher&) = delete;
    Dispatcher& operator=(const Dispatcher&) = delete;
    Dispatcher(Dispatcher&&) = delete;
    Dispatcher& operator=(Dispatcher&&) = delete;

    /// Look `host` up and return the formatted record values.
    ///
    /// @param max_retries  Extra attempts on a retryable failure
    ///                     (single-backend mode only).
    /// @param backoff_ms   Base backoff; attempt `n` waits `backoff_ms * n`.
    [[nodiscard]] coro::Task<std::expected<std::vector<std::string>, domain::DnsErrorInfo>> resolve(
        std::string host, domain::RecordKind kind, std::uint32_t max_retries = DEFAULT_MAX_RETRIES,
        std::uint32_t backoff_ms = DEFAULT_BACKOFF_MS);

    /// Backend count.
    [[nodiscard]] std::size_t size() const noexcept { return resolvers_.size(); }

    [[nodiscard]] Strategy strategy() const noexcept { return strategy_; }

    /// Default retry policy of the port-style entry point.
    static constexpr std::uint32_t DEFAULT_MAX_RETRIES = 1;
    static constexpr std::uint32_t DEFAULT_BACKOFF_MS = 50;
    /// Most backends a concurrent race keeps in flight at once.
    static constexpr std::size_t MAX_CONCURRENT_RESOLVERS = 3;

private:
    /// Single backend with retry.
    [[nodiscard]] coro::Task<std::expected<std::vector<std::string>, domain::DnsErrorInfo>> run_single(
        std::string host, domain::RecordKind kind, std::uint32_t max_retries, std::uint32_t backoff_ms);

    std::vector<std::unique_ptr<Resolver>> resolvers_;
    Strategy strategy_{Strategy::CONCURRENT};
};

}  // namespace dns

#endif  // YADDNSC_INFRASTRUCTURE_DNS_DISPATCHER_H
