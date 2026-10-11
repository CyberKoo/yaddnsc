//
// dns — resolver dispatcher: fallback, shuffle and concurrent strategies.
//
// The concurrent race is the redesign's task_group showcase: at most
// MAX_CONCURRENT_RESOLVERS queries fly at once, each endpoint failure launches
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
/// Failure: expected<records, DnsErrorInfo>. INVALID_QUERY alone ends a search.
/// Endpoint failures and negative answers permit all remaining backends; the
/// first positive answer wins. Without a positive, precedence is INVALID_QUERY,
/// CONFIG, PARSE, operational failure, NODATA, then NXDOMAIN. Single-backend
/// retry uses a cancellable backoff. Multiple backends exhaust alternatives
/// first, then allow at most one recovery round for transient failures.
/// Cancellation throws `coro::Cancelled`; children are cancelled and joined.
/// Thread safety: resolve() runs concurrently, once per subdomain loop, and
/// one dispatcher serves all of them. Safety comes from the backends, each of
/// which serializes its own session — DoT behind an AsyncMutex around the
/// whole session, DoH behind the PersistentClient's mutex, Classic with a
/// socket per query — while the dispatcher itself changes nothing after
/// construction. Encrypted backends include a bounded session wait in their
/// attempt budget; ancestor cancellation remains the overall hard bound.
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
    /// @param max_retries  Extra attempts for one backend. One backend retries
    ///                     while the failure is retryable, up to this many
    ///                     times. Several backends first exhaust the list and
    ///                     then allow at most one recovery round for the
    ///                     CONNECTION/RETRY backends they collected; zero
    ///                     disables that round.
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
