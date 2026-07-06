//
// Created by Kotarou on 2026/6/28.
//

#include "dispatcher.h"

#include <mutex>
#include <thread>
#include <optional>
#include <condition_variable>

#include <spdlog/spdlog.h>
#include <magic_enum/magic_enum.hpp>

#include "fmt.hpp"
#include "dns_error.h"
#include "dns/util.hpp"
#include "util/retry_util.hpp"
#include "dns/proto/parser.h"
#include "dns/resolver/base.h"
#include "exception/dns_lookup.h"

// ===========================================================================
//  ResolverDispatcher::Impl  —  private implementation
// ===========================================================================

struct ResolverDispatcher::Impl {
    Impl(std::vector<std::shared_ptr<ResolverBase> > resolvers, Config::ResolverStrategy strategy)
        : resolvers_(std::move(resolvers)), strategy_(strategy) {
    }

    ~Impl() = default;

    [[nodiscard]] std::vector<std::string>
    resolve(const std::string &host, DNS::Type type, int max_retries, int backoff_ms) const;

    /// Run a single resolver (or the system fallback) with retry logic.
    [[nodiscard]] std::vector<std::string>
    resolve_single(const std::string &host, DNS::Type type, int max_retries, int backoff_ms) const;

    /// Perform a single query attempt using either the system fallback or the sole configured resolver.
    [[nodiscard]] std::vector<std::string>
    resolve_single_attempt(const std::string &host, DNS::Type type) const;

    // ── Shared state for concurrent queries ──
    struct ConcurrentState {
        std::mutex mtx;                            ///< Guards result/error flags.
        std::condition_variable cv;                ///< Notified when a resolver completes.

        std::vector<std::string> result;           ///< Valid result from the fastest resolver.
        bool has_result = false;                   ///< True once a resolver returns a valid answer.

        bool has_nxdomain = false;                 ///< True if any resolver returned NXDOMAIN.
        std::optional<DnsLookupException> definitive_error;  ///< Non-retryable error from any resolver.
        std::optional<DnsLookupException> transient_error;   ///< Retryable error from any resolver.

        int completed = 0;                         ///< Number of resolvers that finished.
        int total = 0;                             ///< Total resolvers in this batch.
    };

    static bool is_retryable(DNS::Error error);

    static void query_resolver(const ResolverBase &resolver, const std::string &host, DNS::Type type,
                               const std::shared_ptr<ConcurrentState> &state);

    [[nodiscard]] std::vector<std::string> resolve_multi(const std::string &host, DNS::Type type) const;

    /// Sequential fallback: try each resolver one by one until one succeeds.
    [[nodiscard]] std::vector<std::string>
    resolve_fallback(const std::string &host, DNS::Type type) const;

    /// Concurrent mode: fire resolvers in batches of 3, take the fastest valid response.
    [[nodiscard]] std::vector<std::string>
    resolve_concurrent(const std::string &host, DNS::Type type) const;

    /// Configured resolver backends (guaranteed non-empty by factory).
    std::vector<std::shared_ptr<ResolverBase> > resolvers_;
    /// Dispatch strategy: FALLBACK (sequential) or CONCURRENT (batched).
    Config::ResolverStrategy strategy_{Config::ResolverStrategy::CONCURRENT};
};

// ===========================================================================
//  ResolverDispatcher::Impl  —  implementations
// ===========================================================================

std::vector<std::string>
ResolverDispatcher::Impl::resolve(const std::string &host, DNS::Type type, int max_retries, int backoff_ms) const {
    // Retry is only applied in single-resolver modes (exactly one resolver).
    // Multi-resolver mode (size > 1) runs without retry — the redundancy of multiple resolvers
    // provides fault tolerance, and retrying the entire multi-resolver round is not desired.
    if (resolvers_.size() == 1) {
        return resolve_single(host, type, max_retries, backoff_ms);
    }

    return resolve_multi(host, type);
}

std::vector<std::string>
ResolverDispatcher::Impl::resolve_single(const std::string &host, DNS::Type type, int max_retries,
                                         int backoff_ms) const {
    unsigned actual_retries = 0;
    auto result = Utils::Retry::retry_on_exception<std::vector<std::string>, DnsLookupException>(
        [&]() -> std::vector<std::string> {
            auto answer = resolve_single_attempt(host, type);

            if (answer.empty()) {
                throw DnsLookupException(
                    fmt::format(R"(DNS lookup for domain "{}" returned no records)", host),
                    DNS::Error::NODATA);
            }

            if (answer.size() > 1) {
                SPDLOG_WARN(R"(Domain "{}" resolved to more than one address (count: {}))", host, answer.size());
            }

            return answer;
        },
        max_retries,
        [](const DnsLookupException &e) { return is_retryable(e.get_error()); },
        backoff_ms,
        &actual_retries
    );

    if (!result) {
        SPDLOG_WARN(R"(DNS lookup for domain "{}" type: {} failed after {} retries. Error: {})", host,
                    magic_enum::enum_name(type), actual_retries, DNS::error_to_str(result.error().get_error()));
        return {};
    }

    return std::move(*result);
}

std::vector<std::string>
ResolverDispatcher::Impl::resolve_single_attempt(const std::string &host, DNS::Type type) const {
    SPDLOG_DEBUG(R"(Using resolver for "{}")", host);
    auto raw = resolvers_[0]->query(host, type);
    auto records = DNS::DnsRecordParser::parse_all(raw.data(), raw.size(), host);
    if (!records.empty()) {
        SPDLOG_DEBUG(R"(DNS lookup for "{}" returned {} record(s): {})", host, records.size(),
                     fmt::join(records, ", "));
    } else {
        SPDLOG_DEBUG(R"(DNS lookup for "{}" returned no records)", host);
    }
    return records;
}

bool ResolverDispatcher::Impl::is_retryable(DNS::Error error) {
    return error == DNS::Error::RETRY || error == DNS::Error::UNKNOWN || error == DNS::Error::CONNECTION;
}

void ResolverDispatcher::Impl::query_resolver(const ResolverBase &resolver, const std::string &host, DNS::Type type,
                                              const std::shared_ptr<ConcurrentState> &state) {
    const auto id = resolver.get_id();
    try {
        auto raw_response = resolver.query(host, type);
        auto records = DNS::DnsRecordParser::parse_all(raw_response.data(), raw_response.size(), host);

        std::lock_guard lock(state->mtx);
        if (!state->has_result && !records.empty()) {
            SPDLOG_DEBUG(R"(Resolver #{} returned {} record(s) for "{}")", id, records.size(), host);
            state->result = std::move(records);
            state->has_result = true;
            state->cv.notify_one();
        }
    } catch (const DnsLookupException &e) {
        std::lock_guard lock(state->mtx);

        if (e.get_error() == DNS::Error::NX_DOMAIN) {
            SPDLOG_DEBUG(R"(Resolver #{} returned NXDOMAIN for "{}")", id, host);
            state->has_nxdomain = true;
            state->definitive_error = e;
        } else {
            SPDLOG_TRACE(R"(Resolver #{} failed for "{}": {})", id, host, DNS::error_to_str(e.get_error()));

            if (is_retryable(e.get_error())) {
                state->transient_error = e;
            } else if (!state->definitive_error.has_value()) {
                state->definitive_error = e;
            }
        }
    } catch (...) {
        {
            std::lock_guard lock(state->mtx);
            SPDLOG_TRACE(R"(Resolver #{} threw an unknown exception for "{}")", id, host);
            if (!state->definitive_error.has_value()) {
                state->definitive_error = DnsLookupException(
                    fmt::format(R"(Resolver #{} threw an unknown exception for "{}")", id, host),
                    DNS::Error::UNKNOWN);
            }
        }
    }

    {
        std::lock_guard lock(state->mtx);
        ++state->completed;
        state->cv.notify_one();
    }
}

std::vector<std::string>
ResolverDispatcher::Impl::resolve_multi(const std::string &host, DNS::Type type) const {
    if (strategy_ == Config::ResolverStrategy::FALLBACK) {
        SPDLOG_DEBUG(R"(Fallback mode: trying {} resolver(s) sequentially for "{}")", resolvers_.size(), host);
        return resolve_fallback(host, type);
    }

    return resolve_concurrent(host, type);
}

std::vector<std::string>
ResolverDispatcher::Impl::resolve_fallback(const std::string &host, DNS::Type type) const {
    DnsLookupException last_error(
        fmt::format(R"(DNS lookup for domain "{}" returned no records)", host),
        DNS::Error::NODATA
    );

    for (const auto &resolver : resolvers_) {
        const auto id = resolver->get_id();
        try {
            auto raw_response = resolver->query(host, type);
            auto result = DNS::DnsRecordParser::parse_all(raw_response.data(), raw_response.size(), host);

            if (!result.empty()) {
                SPDLOG_DEBUG(
                    R"(Fallback resolver #{} returned {} record(s) for "{}": {})", id, result.size(),
                    host, fmt::join(result, ", ")
                );
                return result;
            }

            SPDLOG_DEBUG(R"(Fallback resolver #{} returned no records for "{}")", id, host);
            last_error = DnsLookupException(
                fmt::format(R"(DNS lookup for domain "{}" returned no records)", host),
                DNS::Error::NODATA
            );
        } catch (const DnsLookupException &e) {
            SPDLOG_DEBUG(
                R"(Fallback resolver #{} failed for "{}": {})", id, host, DNS::error_to_str(e.get_error())
            );

            if (e.get_error() == DNS::Error::NX_DOMAIN) {
                throw;
            }

            last_error = e;

            if (!is_retryable(e.get_error())) {
                throw;
            }

            SPDLOG_DEBUG(R"(Fallback resolver #{} returned a retryable error, moving to next)", id);
        }
    }

    if (resolvers_.size() > 1) {
        SPDLOG_ERROR(R"(All {} fallback resolver(s) failed for domain "{}", last error: {})", resolvers_.size(),
                     host, DNS::error_to_str(last_error.get_error())
        );
    }

    throw DnsLookupException(last_error);
}

std::vector<std::string>
ResolverDispatcher::Impl::resolve_concurrent(const std::string &host, DNS::Type type) const {
    constexpr size_t BATCH_SIZE = 3;
    const auto total = resolvers_.size();
    SPDLOG_DEBUG(R"(Concurrent mode: {} resolver(s) for "{}", {} per batch)", total, host, BATCH_SIZE);

    DnsLookupException last_error(
        fmt::format(R"(DNS lookup for domain "{}" returned no records)", host),
        DNS::Error::NODATA
    );

    for (size_t offset = 0; offset < total; offset += BATCH_SIZE) {
        const auto batch_end = std::min(offset + BATCH_SIZE, total);
        const auto batch_count = static_cast<int>(batch_end - offset);

        auto state = std::make_shared<ConcurrentState>();
        state->total = batch_count;

        SPDLOG_DEBUG(R"(Launching batch of {} resolver(s) ({}-{}) for "{}")", batch_count, offset,
                     batch_end - 1, host);

        for (size_t i = offset; i < batch_end; ++i) {
            SPDLOG_TRACE(R"(Batched concurrent resolver #{} for "{}")", resolvers_[i]->get_id(), host);
            std::thread([resolver = resolvers_[i], host, type, state] {
                query_resolver(*resolver, host, type, state);
            }).detach();
        }

        // Wait for the fastest valid result or all threads to finish.
        {
            std::unique_lock lock(state->mtx);
            state->cv.wait(lock, [&state] {
                return state->has_result || state->completed == state->total;
            });
        }

        // Fastest resolver returned a valid result — take it.
        if (state->has_result) {
            SPDLOG_DEBUG(R"(DNS lookup for "{}" returned {} record(s): {})", host, state->result.size(),
                         fmt::join(state->result, ", ")
            );
            return std::move(state->result);
        }

        // All resolvers in the batch finished without a valid result.
        if (state->has_nxdomain) {
            throw DnsLookupException(*state->definitive_error);
        }

        if (state->definitive_error.has_value()) {
            last_error = *state->definitive_error;
            if (!is_retryable(last_error.get_error())) {
                throw DnsLookupException(last_error);
            }
        } else if (state->transient_error.has_value()) {
            last_error = *state->transient_error;
        }
    }

    if (total > 1) {
        SPDLOG_ERROR(R"(All {} resolver(s) failed for domain "{}", last error: {})",
                     total, host, DNS::error_to_str(last_error.get_error()));
    }

    throw DnsLookupException(last_error);
}

// ===========================================================================
//  ResolverDispatcher public API — thin delegation to Impl
// ===========================================================================

ResolverDispatcher::ResolverDispatcher(std::vector<std::shared_ptr<ResolverBase> > resolvers, Config::ResolverStrategy strategy)
    : impl_(std::make_unique<Impl>(std::move(resolvers), strategy)) {
}

ResolverDispatcher::~ResolverDispatcher() = default;

ResolverDispatcher::ResolverDispatcher(ResolverDispatcher &&) noexcept = default;

ResolverDispatcher &ResolverDispatcher::operator=(ResolverDispatcher &&) noexcept = default;

std::vector<std::string>
ResolverDispatcher::resolve(const std::string &host, DNS::Type type, int max_retries, int backoff_ms) const {
    return impl_->resolve(host, type, max_retries, backoff_ms);
}
