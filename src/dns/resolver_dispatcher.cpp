//
// Created by Kotarou on 2026/6/28.
//

#include "resolver_dispatcher.h"

#include <mutex>
#include <thread>
#include <optional>
#include <condition_variable>

#include <spdlog/spdlog.h>
#include <magic_enum/magic_enum.hpp>

#include "base.h"
#include "fmt.hpp"
#include "types.h"
#include "parser.h"
#include "classic.h"
#include "util/retry_util.h"
#include "exception/dns_lookup_exception.h"

// ===========================================================================
//  ResolverDispatcher::Impl  —  private implementation
// ===========================================================================

class ResolverDispatcher::Impl {
public:
    Impl() = default;

    Impl(std::vector<std::shared_ptr<ResolverBase> > resolvers, Config::resolver_strategy strategy)
        : resolvers_(std::move(resolvers)), strategy_(strategy) {
    }

    ~Impl() = default;

    [[nodiscard]] std::vector<std::string>
    resolve(const std::string &host, dns_type type, int max_retries, int backoff_ms) const {
        unsigned actual_retries = 0;
        auto result = Util::retry_on_exception<std::vector<std::string>, DnsLookupException>(
            [&]() -> std::vector<std::string> {
                const std::vector<std::string> dns_answer = [&] {
                    if (resolvers_.empty()) {
                        SPDLOG_TRACE(R"(Using default system resolver for "{}")", host);
                        // Lazily initialize the fallback resolver so it is reused
                        // across retries instead of being recreated every attempt.
                        {
                            std::lock_guard lock(fallback_mtx_);
                            if (!fallback_resolver_) {
                                fallback_resolver_.emplace();
                            }
                        }
                        auto raw_response = fallback_resolver_->query(host, type);
                        auto records = DnsRecordParser::parse_all(raw_response.data(), raw_response.size(), host);
                        if (!records.empty()) {
                            SPDLOG_DEBUG(R"(DNS lookup for "{}" returned {} record(s): {})", host, records.size(),
                                         fmt::join(records, ", "));
                        } else {
                            SPDLOG_DEBUG(R"(DNS lookup for "{}" returned no records)", host);
                        }
                        return records;
                    }

                    if (resolvers_.size() == 1) {
                        SPDLOG_DEBUG(R"(Using single resolver for "{}")", host);
                        auto raw = resolvers_[0]->query(host, type);
                        auto records = DnsRecordParser::parse_all(raw.data(), raw.size(), host);
                        if (!records.empty()) {
                            SPDLOG_DEBUG(R"(DNS lookup for "{}" returned {} record(s): {})", host, records.size(),
                                         fmt::join(records, ", "));
                        } else {
                            SPDLOG_DEBUG(R"(DNS lookup for "{}" returned no records)", host);
                        }
                        return records;
                    }

                    return resolve_multi(host, type);
                }();

                if (dns_answer.empty()) {
                    throw DnsLookupException(
                        fmt::format(R"(DNS lookup for domain "{}" returned no records)", host),
                        dns_error_type::NODATA);
                }

                if (dns_answer.size() > 1) {
                    SPDLOG_WARN(R"(Domain "{}" resolved to more than one address (count: {}))", host,
                                dns_answer.size());
                }

                return dns_answer;
            },
            max_retries,
            [](const DnsLookupException &e) { return e.get_error() == dns_error_type::RETRY; },
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

private:
    // ── Shared state for concurrent queries ──
    struct ConcurrentState {
        std::mutex mtx;
        std::condition_variable cv;

        std::vector<std::string> result;
        bool has_result = false;

        bool has_nxdomain = false;
        std::optional<DnsLookupException> definitive_error;
        std::optional<DnsLookupException> transient_error;

        int completed = 0;
        int total = 0;
    };

    static bool is_retryable(dns_error_type error) {
        return error == dns_error_type::RETRY || error == dns_error_type::UNKNOWN || error ==
               dns_error_type::CONNECTION;
    }

    static void query_resolver(const ResolverBase &resolver, const std::string &host, dns_type type,
                               const std::shared_ptr<ConcurrentState> &state) {
        const auto id = resolver.get_id();
        try {
            auto raw_response = resolver.query(host, type);
            auto records = DnsRecordParser::parse_all(raw_response.data(), raw_response.size(), host);

            std::lock_guard lock(state->mtx);
            if (!state->has_result && !records.empty()) {
                SPDLOG_DEBUG(R"(Resolver #{} returned {} record(s) for "{}")", id, records.size(), host);
                state->result = std::move(records);
                state->has_result = true;
                state->cv.notify_one();
            }
        } catch (const DnsLookupException &e) {
            std::lock_guard lock(state->mtx);

            if (e.get_error() == dns_error_type::NX_DOMAIN) {
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
                        dns_error_type::UNKNOWN);
                }
            }
        }

        {
            std::lock_guard lock(state->mtx);
            ++state->completed;
            state->cv.notify_one();
        }
    }

    [[nodiscard]] std::vector<std::string> resolve_multi(const std::string &host, dns_type type) const {
        // ── Sequential fallback ──
        auto fallback = [&]() -> std::vector<std::string> {
            DnsLookupException last_error(
                fmt::format(R"(DNS lookup for domain "{}" returned no records)", host),
                dns_error_type::NODATA);

            for (const auto &resolver: resolvers_) {
                const auto id = resolver->get_id();
                try {
                    auto raw_response = resolver->query(host, type);
                    auto result = DnsRecordParser::parse_all(raw_response.data(), raw_response.size(), host);

                    if (!result.empty()) {
                        SPDLOG_DEBUG(R"(Fallback resolver #{} returned {} record(s) for "{}": {})", id, result.size(),
                                     host, fmt::join(result, ", "));
                        return result;
                    }

                    SPDLOG_DEBUG(R"(Fallback resolver #{} returned no records for "{}")", id, host);
                    last_error = DnsLookupException(
                        fmt::format(R"(DNS lookup for domain "{}" returned no records)", host),
                        dns_error_type::NODATA);
                } catch (const DnsLookupException &e) {
                    SPDLOG_DEBUG(R"(Fallback resolver #{} failed for "{}": {})", id, host,
                                 DNS::error_to_str(e.get_error()));

                    if (e.get_error() == dns_error_type::NX_DOMAIN) {
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
                             host, DNS::error_to_str(last_error.get_error()));
            }

            throw last_error;
        };

        // ── Concurrent: fire in batches of 3, take fastest valid ──
        auto concurrent = [&]() -> std::vector<std::string> {
            constexpr size_t BATCH_SIZE = 3;
            const auto total = resolvers_.size();
            SPDLOG_DEBUG(R"(Concurrent mode: {} resolver(s) for "{}", {} per batch)", total, host, BATCH_SIZE);

            DnsLookupException last_error(
                fmt::format(R"(DNS lookup for domain "{}" returned no records)", host),
                dns_error_type::NODATA);

            for (size_t offset = 0; offset < total; offset += BATCH_SIZE) {
                const auto batch_end = std::min(offset + BATCH_SIZE, total);
                const auto batch_count = static_cast<int>(batch_end - offset);

                auto state = std::make_shared<ConcurrentState>();
                state->total = batch_count;

                SPDLOG_DEBUG(R"(Launching batch of {} resolver(s) ({}-{}) for "{}")",
                             batch_count, offset, batch_end - 1, host);

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
                                 fmt::join(state->result, ", "));
                    return std::move(state->result);
                }

                // All resolvers in the batch finished without a valid result.
                if (state->has_nxdomain) {
                    throw *state->definitive_error;
                }

                if (state->definitive_error.has_value()) {
                    last_error = *state->definitive_error;
                    if (!is_retryable(last_error.get_error())) {
                        throw last_error;
                    }
                } else if (state->transient_error.has_value()) {
                    last_error = *state->transient_error;
                }
            }

            if (total > 1) {
                SPDLOG_ERROR(R"(All {} resolver(s) failed for domain "{}", last error: {})",
                             total, host, DNS::error_to_str(last_error.get_error()));
            }

            throw last_error;
        };

        // ── Dispatch ──
        if (strategy_ == Config::resolver_strategy::FALLBACK) {
            SPDLOG_DEBUG(R"(Fallback mode: trying {} resolver(s) sequentially for "{}")", resolvers_.size(), host);
            return fallback();
        }

        return concurrent();
    }

    // ── Fallback resolver (used when resolvers_ is empty) ──
    // Initialised lazily so that the ClassicResolver (and its underlying
    // res_ninit) is constructed at most once, even when retries occur.
    mutable std::mutex fallback_mtx_;
    mutable std::optional<ClassicResolver> fallback_resolver_;

    std::vector<std::shared_ptr<ResolverBase> > resolvers_;
    Config::resolver_strategy strategy_{Config::resolver_strategy::CONCURRENT};
};

// ===========================================================================
//  ResolverDispatcher public API — thin delegation to Impl
// ===========================================================================

ResolverDispatcher::ResolverDispatcher() : impl_(std::make_unique<Impl>()) {
}

ResolverDispatcher::ResolverDispatcher(std::vector<std::shared_ptr<ResolverBase> > resolvers,
                                       Config::resolver_strategy strategy)
    : impl_(std::make_unique<Impl>(std::move(resolvers), strategy)) {
}

ResolverDispatcher::~ResolverDispatcher() = default;

ResolverDispatcher::ResolverDispatcher(ResolverDispatcher &&) noexcept = default;

ResolverDispatcher &ResolverDispatcher::operator=(ResolverDispatcher &&) noexcept = default;

std::vector<std::string>
ResolverDispatcher::resolve(const std::string &host, dns_type type, int max_retries, int backoff_ms) const {
    return impl_->resolve(host, type, max_retries, backoff_ms);
}
