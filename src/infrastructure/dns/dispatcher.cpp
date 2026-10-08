//
// dns — resolver dispatcher.
//

#include "dispatcher.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <new>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <magic_enum/magic_enum.hpp>
#include <spdlog/spdlog.h>

#include "domain/error/dns_error.h"
#include "infrastructure/coro/group.hpp"
#include "infrastructure/coro/scope.hpp"
#include "infrastructure/coro/sleep.hpp"
#include "infrastructure/dns/dns_lookup_exception.h"
#include "infrastructure/dns/parser.h"
#include "support/fmt.hpp"
#include "support/util/random.hpp"

namespace dns {
namespace {

/// Process-wide resolver id source; the Dispatcher assigns one per backend so
/// the diagnostics carry the legacy `Resolver #N` label.
std::atomic<std::uint64_t> next_resolver_id{0};

/// Outcome of one backend attempt.
struct Attempt {
    std::uint64_t id = 0;
    bool ok = false;
    std::vector<std::string> records;
    DnsErrorInfo error{DnsError::UNKNOWN, "unclassified DNS failure"};
};

/// Query one backend, parse the answer and classify the RCODE.
///
/// The catch is a boundary translation, not a recovery strategy: a defect from
/// the parser layer becomes an error value, while an allocation failure is
/// rethrown so it cannot masquerade as a retryable DNS error.
[[nodiscard]] coro::Task<Attempt> attempt_one(Resolver& resolver, std::string host, const RecordKind kind) {
    Attempt attempt;
    attempt.id = resolver.id();
    try {
        auto raw = co_await resolver.query(host, kind);
        if (!raw) {
            attempt.error = std::move(raw.error());
            co_return attempt;
        }

        const auto parsed = dns::RecordParser::parse_strings(*raw, host);
        switch (parsed.rcode) {
            case dns::Rcode::NOERROR:
                if (!parsed.records.empty()) {
                    attempt.ok = true;
                    attempt.records = parsed.records;
                    co_return attempt;
                }
                attempt.error = DnsErrorInfo{DnsError::NODATA,
                                             fmt::format(R"(DNS lookup for domain "{}" returned no records)", host)};
                co_return attempt;
            case dns::Rcode::NXDOMAIN:
                attempt.error =
                    DnsErrorInfo{DnsError::NX_DOMAIN, fmt::format(R"(Domain "{}" does not exist (NXDOMAIN))", host)};
                co_return attempt;
            case dns::Rcode::SERVFAIL:
                attempt.error =
                    DnsErrorInfo{DnsError::RETRY, fmt::format(R"(DNS server returned SERVFAIL for "{}")", host)};
                co_return attempt;
            case dns::Rcode::REFUSED:
                attempt.error = DnsErrorInfo{DnsError::SERVER_REFUSED,
                                             fmt::format(R"(DNS server refused the query for "{}")", host)};
                co_return attempt;
            default:
                attempt.error = DnsErrorInfo{DnsError::UNKNOWN, fmt::format(R"(DNS lookup for "{}" returned RCODE {})",
                                                                            host, static_cast<int>(parsed.rcode))};
                co_return attempt;
        }
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const DnsLookupException& error) {
        attempt.error = DnsErrorInfo{error.get_error(), error.what()};
        co_return attempt;
    } catch (const std::exception& error) {
        attempt.error =
            DnsErrorInfo{DnsError::UNKNOWN, fmt::format(R"(DNS lookup for "{}" failed: {})", host, error.what())};
        co_return attempt;
    }
}

/// Collects the diagnosis of a concurrent batch without downgrading it.
///
/// The first definitive error (PARSE/CONFIG) is kept: once one backend has
/// proven the answer unparseable or the configuration broken, a later transient
/// failure must not replace that diagnosis with a retryable one.
class BatchErrors {
public:
    void note(const DnsErrorInfo& error) {
        if (has_definitive_) {
            return;
        }
        if (detail::is_definitive(error.code)) {
            definitive_ = error;
            has_definitive_ = true;
            return;
        }
        if (error.code == DnsError::NX_DOMAIN) {
            if (!has_nxdomain_) {
                nxdomain_ = error;
                has_nxdomain_ = true;
            }
            return;
        }
        transient_ = error;
    }

    [[nodiscard]] bool has_definitive() const noexcept { return has_definitive_; }

    [[nodiscard]] bool has_nxdomain() const noexcept { return has_nxdomain_; }

    [[nodiscard]] DnsErrorInfo best(const std::string& host) const {
        if (has_definitive_) {
            return definitive_;
        }
        if (has_nxdomain_) {
            return nxdomain_;
        }
        if (transient_.code != DnsError::UNKNOWN || !transient_.message.empty()) {
            return transient_;
        }
        return DnsErrorInfo{DnsError::NODATA, fmt::format(R"(DNS lookup for domain "{}" returned no records)", host)};
    }

private:
    DnsErrorInfo definitive_;
    DnsErrorInfo nxdomain_;
    DnsErrorInfo transient_{DnsError::NODATA, {}};
    bool has_definitive_ = false;
    bool has_nxdomain_ = false;
};

/// Race one batch: the first backend to answer wins and cancels the rest.
[[nodiscard]] coro::Task<std::expected<std::vector<std::string>, DnsErrorInfo>> run_concurrent_batch(
    const std::span<Resolver* const> batch, std::string host, const RecordKind kind) {
    std::optional<std::vector<std::string>> winner;
    BatchErrors errors;
    bool cancelled = false;

    SPDLOG_DEBUG(R"(Launching batch of {} resolver(s) for "{}")", batch.size(), host);

    // A child scope lets the winner cancel its siblings while the group still
    // joins and reaps every child on exit.
    co_await coro::with_cancel_scope([&](coro::CancelScope& race) -> coro::Task<void> {
        co_await coro::task_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            for (Resolver* resolver : batch) {
                group.spawn(attempt_one(*resolver, host, kind));
            }
            // Completion order decides, not launch order.
            while (auto outcome = co_await group.next<Attempt>()) {
                if (!outcome->has_value()) {
                    break;  // a child threw: the group rethrows after joining
                }
                const Attempt& attempt = outcome->value();
                if (attempt.ok) {
                    SPDLOG_DEBUG(R"(Resolver #{} returned {} record(s) for "{}")", attempt.id,
                                 attempt.records.size(), host);
                    winner = attempt.records;
                    race.cancel();  // wake the losers; scope exit joins them
                    break;
                }
                if (attempt.error.code == DnsError::CANCELLED) {
                    SPDLOG_TRACE(R"(Resolver #{} cancelled for "{}")", attempt.id, host);
                    cancelled = true;
                    continue;  // keep consuming until every child has finished
                }
                if (attempt.error.code == DnsError::NX_DOMAIN) {
                    SPDLOG_DEBUG(R"(Resolver #{} returned NXDOMAIN for "{}")", attempt.id, host);
                } else if (detail::is_definitive(attempt.error.code)) {
                    SPDLOG_TRACE(R"(Resolver #{} failed for "{}": {})", attempt.id, host,
                                 error_to_str(attempt.error.code));
                } else {
                    SPDLOG_TRACE(R"(Resolver #{} returned {} for "{}")", attempt.id, host,
                                 error_to_str(attempt.error.code));
                }
                errors.note(attempt.error);
            }
            co_return;
        });
    });

    if (winner.has_value()) {
        co_return std::move(*winner);
    }
    if (cancelled) {
        co_return std::unexpected(DnsErrorInfo{DnsError::CANCELLED, "DNS batch cancelled"});
    }
    co_return std::unexpected(errors.best(host));
}

/// Walk backends in order; stop on any answer or definitive failure.
[[nodiscard]] coro::Task<std::expected<std::vector<std::string>, DnsErrorInfo>> run_sequential(
    const std::span<Resolver* const> order, std::string host, const RecordKind kind) {
    DnsErrorInfo last{DnsError::NODATA, fmt::format(R"(DNS lookup for domain "{}" returned no records)", host)};

    for (Resolver* resolver : order) {
        auto attempt = co_await attempt_one(*resolver, host, kind);
        const auto id = attempt.id;
        if (attempt.ok) {
            if (attempt.records.size() > 1) {
                SPDLOG_WARN(R"(Resolver #{} Domain "{}" resolved to more than one address (count: {}))", id, host,
                            attempt.records.size());
            }
            SPDLOG_DEBUG(R"(Fallback resolver #{} returned {} record(s) for "{}": {})", id, attempt.records.size(),
                         host, fmt::join(attempt.records, ", "));
            co_return std::move(attempt.records);
        }

        SPDLOG_DEBUG(R"(Fallback resolver #{} failed for "{}": {})", id, host, error_to_str(attempt.error.code));
        last = std::move(attempt.error);
        if (detail::is_definitive(last.code) || last.code == DnsError::NX_DOMAIN || last.code == DnsError::CANCELLED) {
            co_return std::unexpected(std::move(last));
        }
        SPDLOG_DEBUG(R"(Fallback resolver #{} returned a retryable error, moving to next)", id);
    }
    if (order.size() > 1) {
        SPDLOG_ERROR(R"(All {} fallback resolver(s) failed for domain "{}", last error: {})", order.size(), host,
                     error_to_str(last.code));
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

coro::Task<std::expected<std::vector<std::string>, DnsErrorInfo>> Dispatcher::resolve(std::string host,
                                                                                      const RecordKind kind,
                                                                                      const std::uint32_t max_retries,
                                                                                      const std::uint32_t backoff_ms) {
    if (resolvers_.empty()) {
        co_return std::unexpected(DnsErrorInfo{DnsError::CONFIG, "no DNS resolvers are configured"});
    }

    std::vector<Resolver*> all;
    all.reserve(resolvers_.size());
    for (const auto& resolver : resolvers_) {
        all.push_back(resolver.get());
    }

    if (all.size() == 1) {
        co_return co_await run_single(host, kind, max_retries, backoff_ms);
    }

    if (strategy_ == Strategy::CONCURRENT) {
        SPDLOG_DEBUG(R"(Concurrent mode: {} resolver(s) for "{}", {} per batch)", all.size(), host,
                     MAX_CONCURRENT_RESOLVERS);
        DnsErrorInfo last{DnsError::NODATA, fmt::format(R"(DNS lookup for domain "{}" returned no records)", host)};
        for (std::size_t offset = 0; offset < all.size(); offset += MAX_CONCURRENT_RESOLVERS) {
            const auto end = std::min(offset + MAX_CONCURRENT_RESOLVERS, all.size());
            auto batch = std::span(all).subspan(offset, end - offset);
            auto result = co_await run_concurrent_batch(batch, host, kind);
            if (result.has_value()) {
                co_return std::move(*result);
            }
            last = std::move(result.error());
            // Definitive diagnoses end the search; a batch that simply failed
            // moves on to the next group of backends.
            if (detail::is_definitive(last.code) || last.code == DnsError::NX_DOMAIN ||
                last.code == DnsError::CANCELLED) {
                co_return std::unexpected(std::move(last));
            }
        }
        if (all.size() > 1) {
            SPDLOG_ERROR(R"(All {} resolver(s) failed for domain "{}", last error: {})", all.size(), host,
                         error_to_str(last.code));
        }
        co_return std::unexpected(std::move(last));
    }

    if (strategy_ == Strategy::SHUFFLE) {
        SPDLOG_DEBUG(R"(Shuffle mode: trying {} resolver(s) in random order for "{}")", all.size(), host);
        std::vector<Resolver*> shuffled = all;
        std::ranges::shuffle(shuffled, Utils::Random::engine());
        co_return co_await run_sequential(shuffled, host, kind);
    }

    SPDLOG_DEBUG(R"(Fallback mode: trying {} resolver(s) in configured order for "{}")", all.size(), host);
    co_return co_await run_sequential(all, host, kind);
}

coro::Task<std::expected<std::vector<std::string>, DnsErrorInfo>> Dispatcher::run_single(
    std::string host, const RecordKind kind, const std::uint32_t max_retries, const std::uint32_t backoff_ms) {
    for (std::uint32_t attempt_index = 0;; ++attempt_index) {
        auto attempt = co_await attempt_one(*resolvers_.front(), host, kind);
        if (attempt.ok) {
            if (attempt.records.size() > 1) {
                SPDLOG_WARN(R"(Domain "{}" resolved to more than one address (count: {}))", host,
                            attempt.records.size());
            }
            co_return std::move(attempt.records);
        }
        if (attempt.error.code == DnsError::CANCELLED || !detail::is_retryable(attempt.error.code) ||
            attempt_index >= max_retries) {
            if (attempt.error.code == DnsError::NODATA) {
                SPDLOG_DEBUG(R"(DNS lookup for "{}" returned no records)", host);
            } else {
                SPDLOG_WARN(R"(DNS lookup for domain "{}" type: {} failed after {} retries. Error: {})", host,
                            magic_enum::enum_name(kind), attempt_index, error_to_str(attempt.error.code));
            }
            co_return std::unexpected(std::move(attempt.error));
        }

        SPDLOG_DEBUG("retrying... (counter {})", attempt_index + 1);

        // Backoff is a cancellable sleep: a cancelled scope aborts the wait and
        // the query, with no thread and no derived cancellation domain.
        const auto slept = co_await coro::sleep_for(std::chrono::milliseconds(backoff_ms) * (attempt_index + 1));
        if (!slept.has_value()) {
            co_return std::unexpected(DnsErrorInfo{DnsError::CANCELLED, "DNS retry backoff cancelled"});
        }
    }
}

}  // namespace dns
