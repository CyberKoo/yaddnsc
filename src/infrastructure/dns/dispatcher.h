//
// dns — resolver dispatcher: fallback, shuffle and concurrent strategies.
//
// The concurrent race uses a nursery to bound execution: at most
// the configured concurrency limit queries fly at once; each endpoint failure launches
// the next backend at once, the winner cancels its siblings through a child
// cancel scope, and scope exit still joins and reaps every child (design §3.2).
// Retry backoff is a cancellable sleep, not a thread.
//

#ifndef YADDNSC_INFRASTRUCTURE_DNS_DISPATCHER_H
#define YADDNSC_INFRASTRUCTURE_DNS_DISPATCHER_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include "coro/task.hpp"
#include "domain/error/dns_error_info.h"
#include "infrastructure/dns/policy.h"
#include "infrastructure/dns/resolver/resolver.h"

namespace domain {
enum class RecordKind;
}  // namespace domain

namespace dns {

/// How a dispatcher uses its backends.
enum class Strategy {
    FALLBACK,    ///< Try backends in configured order until one answers.
    SHUFFLE,     ///< Same, in a random order per query.
    CONCURRENT,  ///< Race backends within the policy limit; the first answer wins.
};

/// Composes resolver backends behind one query operation.
///
/// Ownership: owns the backends.
/// Failure: expected<records, DnsErrorInfo>. INVALID_QUERY alone ends a search.
/// Endpoint failures and negative answers permit all remaining backends; the
/// first positive answer wins. Without a positive, precedence is INVALID_QUERY,
/// CONFIG, PARSE, operational failure, NODATA, then NXDOMAIN. Single-backend
/// retry uses a cancellable backoff. Multiple backends exhaust alternatives
/// first, then allow at most one recovery round for transient failures.
/// Input preflight does not acquire entropy. Backend secure-ID failures return
/// CONNECTION and participate in the normal failover/retry policy.
/// Cancellation throws `coro::Cancelled`; children are cancelled and joined.
/// Thread safety: resolve() runs concurrently, once per subdomain task, and
/// one dispatcher serves all of them. Safety comes from the backends, each of
/// which serializes its own session — DoT behind an AsyncMutex around the
/// whole session, DoH behind the PersistentClient's mutex, Classic with a
/// socket per query — while the dispatcher itself changes nothing after
/// construction. Encrypted backends wait for admission before their attempt
/// starts; caller cancellation bounds that wait. Internal defects propagate.
class Dispatcher {
public:
    explicit Dispatcher(std::vector<std::unique_ptr<Resolver>> resolvers, Strategy strategy = Strategy::CONCURRENT,
                        Policy policy = {});

    ~Dispatcher();

    Dispatcher(const Dispatcher&) = delete;
    Dispatcher& operator=(const Dispatcher&) = delete;
    Dispatcher(Dispatcher&&) = delete;
    Dispatcher& operator=(Dispatcher&&) = delete;

    /// Look `host` up and return the formatted record values, using the retry
    /// policy supplied at construction: one backend retries while the failure
    /// is retryable, up to `Policy::max_retries` times; several backends first
    /// exhaust the list and then allow at most one recovery round for the
    /// CONNECTION/RETRY backends they collected; zero disables that round.
    /// Attempt `n` waits `Policy::retry_backoff * n` before retrying.
    [[nodiscard]] coro::Task<std::expected<std::vector<std::string>, domain::DnsErrorInfo>> resolve(
        std::string host, domain::RecordKind kind);

    /// Backend count.
    [[nodiscard]] std::size_t size() const noexcept { return resolvers_.size(); }

    [[nodiscard]] Strategy strategy() const noexcept { return strategy_; }

private:
    /// Single backend with retry.
    [[nodiscard]] coro::Task<std::expected<std::vector<std::string>, domain::DnsErrorInfo>> run_single(
        std::string host, domain::RecordKind kind, std::uint32_t max_retries, std::chrono::milliseconds backoff);

    std::vector<std::unique_ptr<Resolver>> resolvers_;
    Strategy strategy_{Strategy::CONCURRENT};
    Policy policy_;
};

}  // namespace dns

#endif  // YADDNSC_INFRASTRUCTURE_DNS_DISPATCHER_H
