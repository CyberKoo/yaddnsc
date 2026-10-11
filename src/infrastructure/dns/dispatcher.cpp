//
// dns — resolver dispatcher.
//

#include "dispatcher.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <new>
#include <optional>
#include <ratio>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <coroutine>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not

#include <expected>
#include <magic_enum/magic_enum.hpp>
#include <spdlog/spdlog.h>

#include "coro/cancel_scope.h"
#include "coro/cancelled.h"
#include "coro/group.hpp"
#include "coro/scope.hpp"
#include "coro/scope_outcome.hpp"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include "coro/sleep.hpp"
#include "coro/task_group.hpp"
#include "domain/error/dns_error.h"
#include "infrastructure/dns/dns_lookup_exception.h"
#include "infrastructure/dns/dns_packet_exception.h"
#include "infrastructure/dns/parser.h"
#include "infrastructure/dns/resolver/resolver.h"
#include "infrastructure/dns/types.h"
#include "infrastructure/dns/util.hpp"
#include "infrastructure/dns/wire/query_util.h"
#include "infrastructure/logging/level_enabled.h"
#include "support/fmt.hpp"
#include "support/util/random.hpp"
#include "yaddnsc/util/format.hpp"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not

namespace domain {
enum class RecordKind;
}  // namespace domain

namespace dns {
namespace {

/// Process-wide resolver id source; the Dispatcher assigns one per backend so
/// the diagnostics carry the legacy `Resolver #N` label.
std::atomic<std::uint64_t> next_resolver_id{0};

/// Outcome of one backend attempt.
struct Attempt {
    Resolver* backend = nullptr;  // Borrowed from the dispatcher; groups join before its destruction.
    std::uint64_t id = 0;
    bool ok = false;
    std::vector<std::string> records;
    domain::DnsErrorInfo error{domain::DnsError::UNKNOWN, "unclassified DNS failure"};
};

/// Query one backend, parse the answer and classify the RCODE.
///
/// The catch is a boundary translation, not a recovery strategy: a defect from
/// the parser layer becomes an error value, while an allocation failure is
/// rethrown so it cannot masquerade as a retryable DNS error.
[[nodiscard]] coro::Task<Attempt> attempt_one(Resolver& resolver, std::string host, const domain::RecordKind kind) {
    Attempt attempt;
    attempt.backend = &resolver;
    attempt.id = resolver.id();
    try {
        auto raw = co_await resolver.query(host, kind);
        if (!raw) {
            attempt.error = std::move(raw.error());
            co_return attempt;
        }

        const auto parsed = dns::RecordParser::parse_strings(*raw, host, dns::Util::type_to_record_type(kind));
        if (auto error = dns::Util::rcode_error(parsed.rcode, host)) {
            attempt.error = std::move(*error);
        } else if (!parsed.records.empty()) {
            attempt.ok = true;
            attempt.records = parsed.records;
        } else {
            attempt.error = domain::DnsErrorInfo{
                domain::DnsError::NODATA, fmt::format(R"(DNS lookup for "{}" returned no usable records)", host)};
        }
        co_return attempt;
    } catch (const coro::Cancelled&) {
        // Race losers unwind here when the winner cancels the scope. Trace and
        // rethrow: cancellation is control flow, and this line is the only
        // record that the losing upstream was discarded, not stuck.
        SPDLOG_TRACE(R"(Resolver #{} cancelled while querying "{}")", attempt.id, host);
        throw;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const DnsLookupException& error) {
        attempt.error = domain::DnsErrorInfo{error.get_error(), error.what()};
        co_return attempt;
    } catch (const std::exception& error) {
        // A defect reaching here escaped the resolver and the parser without
        // being classified: the error value below hides it from the caller, so
        // the boundary records it before translating.
        SPDLOG_WARN(R"(Unexpected exception from resolver #{} for "{}": {})", attempt.id, host, error.what());
        attempt.error = domain::DnsErrorInfo{domain::DnsError::UNKNOWN,
                                             fmt::format(R"(DNS lookup for "{}" failed: {})", host, error.what())};
        co_return attempt;
    }
}

/// The same failure precedence applies to sequential and concurrent searches.
class BatchErrors {
public:
    void note(const Attempt& attempt) {
        // A recovery result supersedes that backend's earlier transient failure.
        errors_.insert_or_assign(attempt.id, attempt.error);
    }

    [[nodiscard]] domain::DnsErrorInfo best(const std::string& host) const {
        if (errors_.empty())
            return {domain::DnsError::UNKNOWN, fmt::format(R"(No resolver reported a diagnosis for "{}")", host)};
        auto result = errors_.begin()->second;
        std::size_t nxdomains = 0;
        std::size_t nodata = 0;
        for (const auto& entry : errors_) {
            const auto& error = entry.second;
            nxdomains += error.code == domain::DnsError::NX_DOMAIN ? 1U : 0U;
            nodata += error.code == domain::DnsError::NODATA ? 1U : 0U;
            if (Util::error_priority(error.code) > Util::error_priority(result.code))
                result = error;
        }
        if ((nxdomains != 0 && result.code != domain::DnsError::NX_DOMAIN) ||
            (nodata != 0 && result.code != domain::DnsError::NODATA)) {
            result.message += fmt::format("; negative responses: NXDOMAIN={}, NODATA={}", nxdomains, nodata);
        }
        return result;
    }

private:
    std::map<std::uint64_t, domain::DnsErrorInfo> errors_;
};

/// Race every backend with at most MAX_CONCURRENT_RESOLVERS queries in flight.
///
/// The first answer wins and cancels the in-flight losers; every transient
/// failure frees its slot and launches the next backend at once instead of
/// waiting for a whole wave to settle. Negative answers and endpoint failures
/// free slots too; only INVALID_QUERY ends the search without a positive answer.
[[nodiscard]] coro::Task<std::expected<std::vector<std::string>, domain::DnsErrorInfo>> run_concurrent_race(
    const std::span<Resolver* const> resolvers, std::string host, const domain::RecordKind kind, BatchErrors& errors,
    std::vector<Resolver*>& retry) {
    std::optional<std::vector<std::string>> winner;

    // A child scope lets the winner cancel its siblings while the group still
    // joins and reaps every child on exit.
    co_await coro::with_cancel_scope([&](coro::CancelScope& race) -> coro::Task<void> {
        co_await coro::task_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            auto pending = resolvers.begin();
            const auto launch = [&group, &pending, resolvers, &host, kind] {
                if (pending != resolvers.end()) {
                    group.spawn(attempt_one(**pending, host, kind));
                    ++pending;
                }
            };
            for (std::size_t i = 0; i < Dispatcher::MAX_CONCURRENT_RESOLVERS; ++i) {
                launch();
            }
            // Completion order decides, not launch order.
            while (auto outcome = co_await group.next<Attempt>()) {
                // A defective child can never surface here: its completion
                // cancels this group scope before next() is woken, so the next
                // call raises Cancelled at its checkpoint and run_group rethrows
                // the defect after the join. Cancelled children are skipped by
                // next() itself, so every outcome below carries a value.
                const Attempt& attempt = outcome->value();
                if (attempt.ok) {
                    SPDLOG_TRACE(R"(Resolver #{} returned {} record(s) for "{}")", attempt.id, attempt.records.size(),
                                 host);
                    winner = attempt.records;
                    race.cancel();  // wake the losers; scope exit joins them
                    break;
                }

                if (attempt.error.code == domain::DnsError::NX_DOMAIN) {
                    SPDLOG_TRACE(R"(Resolver #{} returned NXDOMAIN for "{}")", attempt.id, host);
                } else if (detail::is_definitive(attempt.error.code)) {
                    SPDLOG_TRACE(R"(Resolver #{} failed for "{}": {})", attempt.id, host,
                                 domain::error_to_str(attempt.error.code));
                } else {
                    SPDLOG_TRACE(R"(Resolver #{} returned {} for "{}")", attempt.id,
                                 domain::error_to_str(attempt.error.code), host);
                }
                errors.note(attempt);
                if (detail::is_definitive(attempt.error.code)) {
                    race.cancel();
                    break;
                }
                if (attempt.error.code == domain::DnsError::CONNECTION ||
                    attempt.error.code == domain::DnsError::RETRY) {
                    retry.push_back(attempt.backend);
                }
                launch();  // every endpoint failure/negative answer frees its slot
            }
            co_return;
        });
    });

    if (winner.has_value()) {
        co_return std::move(*winner);
    }

    co_return std::unexpected(errors.best(host));
}

/// Walk backends in order; stop on any answer or definitive failure.
[[nodiscard]] coro::Task<std::expected<std::vector<std::string>, domain::DnsErrorInfo>> run_sequential(
    const std::span<Resolver* const> order, std::string host, const domain::RecordKind kind, BatchErrors& errors,
    std::vector<Resolver*>& retry) {
    for (Resolver* resolver : order) {
        auto attempt = co_await attempt_one(*resolver, host, kind);
        const auto id = attempt.id;
        if (attempt.ok) {
            if (attempt.records.size() > 1) {
                SPDLOG_DEBUG(R"(Resolver #{} Domain "{}" resolved to more than one address (count: {}))", id, host,
                             attempt.records.size());
            }
            // fmt::join copies the records into a temporary — build it only for a written record.
            if (logging::level_enabled(spdlog::level::trace)) {
                SPDLOG_TRACE(R"(Fallback resolver #{} returned {} record(s) for "{}": {})", id, attempt.records.size(),
                             host, fmt::join(attempt.records, ", "));
            }
            co_return std::move(attempt.records);
        }

        SPDLOG_TRACE(R"(Fallback resolver #{} failed for "{}": {})", id, host,
                     domain::error_to_str(attempt.error.code));
        if (detail::is_definitive(attempt.error.code)) {
            co_return std::unexpected(std::move(attempt.error));
        }
        errors.note(attempt);
        if (attempt.error.code == domain::DnsError::CONNECTION || attempt.error.code == domain::DnsError::RETRY)
            retry.push_back(resolver);
        SPDLOG_TRACE(R"(Fallback resolver #{} could not answer, moving to next)", id);
    }
    auto last = errors.best(host);
    if (order.size() > 1) {
        if (last.code == domain::DnsError::NODATA) {
            // NOERROR without answers is the state of every name before its
            // first DDNS update, so it is an answer rather than a failure.
            SPDLOG_TRACE(R"(All {} fallback resolver(s) returned no records for "{}")", order.size(), host);
        } else {
            SPDLOG_WARN(R"(All {} fallback resolver(s) failed for "{}": {})", order.size(), host,
                        domain::error_to_str(last.code));
        }
    }
    co_return std::unexpected(std::move(last));
}

}  // namespace

Dispatcher::Dispatcher(std::vector<std::unique_ptr<Resolver>> resolvers, const Strategy strategy)
    : resolvers_(std::move(resolvers)), strategy_(strategy) {
    for (const auto& resolver : resolvers_) {
        resolver->set_id(next_resolver_id.fetch_add(1, std::memory_order_relaxed));
    }
}

Dispatcher::~Dispatcher() = default;

coro::Task<std::expected<std::vector<std::string>, domain::DnsErrorInfo>> Dispatcher::resolve(
    std::string host, const domain::RecordKind kind, const std::uint32_t max_retries, const std::uint32_t backoff_ms) {
    if (resolvers_.empty()) {
        co_return std::unexpected(domain::DnsErrorInfo{domain::DnsError::CONFIG, "no DNS resolvers are configured"});
    }

    if (host.empty())
        host = ".";  // Root name, not the parser's unfiltered diagnostic mode.
    // Map a local input defect to INVALID_QUERY before any backend is reached.
    // Every backend still builds its own query — the transaction ID must differ
    // per packet — so the win is failing the search here rather than after a
    // backend has already queued, built and sent a request.
    try {
        [[maybe_unused]] const auto checked = build_query(host, Util::type_to_record_type(kind));
    } catch (const DnsPacketException& error) {
        co_return std::unexpected(domain::DnsErrorInfo{domain::DnsError::INVALID_QUERY, error.what()});
    }

    std::vector<Resolver*> all;
    all.reserve(resolvers_.size());
    for (const auto& resolver : resolvers_) {
        all.push_back(resolver.get());
    }

    if (all.size() == 1) {
        co_return co_await run_single(host, kind, max_retries, backoff_ms);
    }

    if (strategy_ == Strategy::SHUFFLE)
        std::ranges::shuffle(all, Utils::Random::engine());
    BatchErrors errors;
    // Exhaust alternatives first. At most one recovery round for transient
    // failures, never for malformed/negative answers. Ancestor budgets still
    // cancel long candidate lists; no nested backend retry multiplication.
    for (std::uint32_t round = 0;; ++round) {
        std::vector<Resolver*> retry;
        auto result = strategy_ == Strategy::CONCURRENT ? co_await run_concurrent_race(all, host, kind, errors, retry)
                                                        : co_await run_sequential(all, host, kind, errors, retry);
        if (result || detail::is_definitive(result.error().code) || retry.empty() ||
            round >= std::min(max_retries, 1U)) {
            co_return result;
        }
        co_await coro::sleep_for(std::chrono::milliseconds(backoff_ms));
        all = std::move(retry);
    }
}

coro::Task<std::expected<std::vector<std::string>, domain::DnsErrorInfo>> Dispatcher::run_single(
    std::string host, const domain::RecordKind kind, const std::uint32_t max_retries, const std::uint32_t backoff_ms) {
    for (std::uint32_t attempt_index = 0;; ++attempt_index) {
        auto attempt = co_await attempt_one(*resolvers_.front(), host, kind);
        if (attempt.ok) {
            if (attempt.records.size() > 1) {
                SPDLOG_DEBUG(R"(Domain "{}" resolved to more than one address (count: {}))", host,
                             attempt.records.size());
            }
            co_return std::move(attempt.records);
        }
        if (!detail::is_retryable(attempt.error.code) || attempt_index >= max_retries) {
            if (attempt.error.code == domain::DnsError::NODATA) {
                SPDLOG_TRACE(R"(DNS lookup for "{}" returned no records)", host);
            } else {
                SPDLOG_WARN(R"(DNS lookup for "{}" ({}) failed after {} retries: {})", host,
                            magic_enum::enum_name(kind), attempt_index, domain::error_to_str(attempt.error.code));
            }
            co_return std::unexpected(std::move(attempt.error));
        }

        SPDLOG_TRACE("Retrying... (counter {})", attempt_index + 1);

        // Backoff is a cancellable sleep: a cancelled scope aborts the wait and
        // the query, with no thread and no derived cancellation domain.
        co_await coro::sleep_for(std::chrono::milliseconds(backoff_ms) * (attempt_index + 1));
    }
}

}  // namespace dns
