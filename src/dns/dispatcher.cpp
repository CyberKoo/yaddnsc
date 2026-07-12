//
// Created by Kotarou on 2026/6/28.
//
// Native resolver dispatcher — jthread-based concurrent dispatch with
// per-thread cancellation pipes and no shared mutable state.
//
// Compiled when YADDNSC_USE_NATIVE_DNS=1.
//

#include "dispatcher.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <future>
#include <numeric>
#include <stop_token>
#include <thread>
#include <utility>

#include "dns/parser/parser.h"
#include "dns/resolver/base.h"
#include "dns/dns_error_info.h"
#include "exception/dns_lookup.h"
#include "util/cancellation_token.hpp"
#include "util/fd.hpp"
#include "util/random.hpp"
#include "util/retry_util.hpp"

#include "dns_error.h"

#include "fmt.hpp"
#include <expected>
#include <magic_enum/magic_enum.hpp>
#include <spdlog/spdlog.h>
#include <unistd.h>

// ===========================================================================
//  Anonymous namespace  —  stateless utility functions
// ===========================================================================

namespace {
    /// Check whether a DNS error is transient and should be retried.
    [[nodiscard]] bool is_retryable(DnsError error) { // NOLINT(misc-use-internal-linkage)
        return error == DnsError::RETRY || error == DnsError::UNKNOWN || error == DnsError::CONNECTION;
    }

    /// Query a single resolver, parse the response, and classify the result.
    /// Handles transport errors, parse exceptions, and all RCODE values uniformly.
    ///
    /// @note  The outer try-catch is a safety barrier at the dispatcher boundary.
    ///        Unexpected exceptions from the resolver/parser layer are caught here
    ///        to prevent std::terminate — this is NOT catch-to-convert for flow control.
    [[nodiscard]] std::expected<std::vector<std::string>, DnsErrorInfo>
    try_resolve(const ResolverBase &resolver, const std::string &host, RecordKind type,
                const Utils::CancellationToken &cancel_token) {
        try {
            // ── 1. Query the resolver (transport layer) ──
            auto raw = resolver.query(host, type, cancel_token);
            if (!raw) {
                return std::unexpected(std::move(raw.error()));
            }

            // ── 2. Parse the raw response ──
            // RecordParser::parse_strings forms a deep call chain (5+ levels) where all
            // errors are DnsLookupException(PARSE).  Caught by the first outer catch below.
            auto parsed = DNS::RecordParser::parse_strings(*raw, host);

            // ── 3. Classify by RCODE ──
            switch (parsed.rcode) {
                case DNS::Rcode::NOERROR:
                    if (!parsed.records.empty()) {
                        return std::move(parsed.records);
                    }
                    return std::unexpected(DnsErrorInfo{
                        DnsError::NODATA,
                        fmt::format(R"(DNS lookup for domain "{}" returned no records)", host)
                    });

                case DNS::Rcode::NXDOMAIN:
                    return std::unexpected(DnsErrorInfo{
                        DnsError::NX_DOMAIN,
                        fmt::format(R"(Domain "{}" does not exist (NXDOMAIN))", host)
                    });

                case DNS::Rcode::SERVFAIL:
                    return std::unexpected(DnsErrorInfo{
                        DnsError::RETRY,
                        fmt::format(R"(DNS server returned SERVFAIL for "{}")", host)
                    });

                case DNS::Rcode::REFUSED:
                    return std::unexpected(DnsErrorInfo{
                        DnsError::SERVER_REFUSED,
                        fmt::format(R"(DNS server refused query for "{}")", host)
                    });

                default:
                    return std::unexpected(DnsErrorInfo{
                        DnsError::UNKNOWN,
                        fmt::format(R"(DNS lookup for "{}" returned RCODE {})", host,
                                    magic_enum::enum_name(parsed.rcode))
                    });
            }
        } catch (const DnsLookupException &e) {
            // Deep-call-chain boundary translation: parser internals (5+ levels)
            // throw DnsLookupException(PARSE).  Preserve the error code.
            return std::unexpected(DnsErrorInfo{e.get_error(), e.what()});
        } catch (const std::exception &e) {
            return std::unexpected(DnsErrorInfo{
                DnsError::UNKNOWN,
                fmt::format(R"(DNS lookup for "{}" failed: {})", host, e.what())
            });
        }
    }
} // anonymous namespace

// ===========================================================================
//  Class declarations  —  SingleResolverRunner, FallbackRunner, BatchRunner,
//                          ConcurrentRunner
// ===========================================================================

/// Single-resolver dispatch with retry.
class SingleResolverRunner {
public:
    explicit SingleResolverRunner(const ResolverBase &resolver);

    [[nodiscard]] std::expected<std::vector<std::string>, DnsErrorInfo>
    run(const std::string &host, RecordKind type, std::uint32_t max_retries, std::uint32_t backoff_ms) const;

private:
    const ResolverBase &resolver_;
};

/// Sequential fallback across all resolvers.
///
/// Iterates resolvers in random order and returns the first successful
/// result.  On definitive errors (NXDOMAIN, PARSE, CONFIG) the iteration
/// stops immediately and the error is propagated via std::expected.
class FallbackRunner {
public:
    explicit FallbackRunner(const std::vector<std::unique_ptr<ResolverBase> > &resolvers);

    [[nodiscard]] std::expected<std::vector<std::string>, DnsErrorInfo> run(
        const std::string &host, RecordKind type) const;

private:
    const std::vector<std::unique_ptr<ResolverBase> > &resolvers_;
};

/// Executes one batch of concurrent resolver queries with cancellation pipes.
///
/// On success returns the resolved records (non-empty vector).  On failure
/// returns std::unexpected with the appropriate DnsErrorInfo — callers
/// should check the error code to distinguish definitive errors
/// (NXDOMAIN, PARSE, CONFIG) from transient ones (RETRY, CONNECTION).
class BatchRunner {
public:
    explicit BatchRunner(const std::string &host, RecordKind type) noexcept;

    [[nodiscard]] std::expected<std::vector<std::string>, DnsErrorInfo>
    run(std::span<const std::unique_ptr<ResolverBase>> batch);

private:
    void resolve_one(const std::stop_token &st, const ResolverBase &resolver);

    /// Signal completion by setting the promise to an empty vector when the
    /// last in-flight resolver finishes (prev reaches batch_count_ - 1).
    /// Safe to call multiple times — future_error is caught if the promise
    /// was already satisfied by set_promise_value().
    void signal_completion(int prev) noexcept;

    /// Set the promise value, catching future_error if another thread
    /// already set it (e.g. a faster resolver answered first).
    void set_promise_value(std::vector<std::string> value) noexcept;

    const std::string &host_;
    RecordKind type_;

    // ── Per-batch state (reset at the start of each run()) ──
    std::atomic<int> completed_{0};
    std::atomic<bool> has_nxdomain_{false};
    std::atomic<bool> has_definitive_{false};
    std::atomic<DnsError> batch_error_{DnsError::NODATA};
    std::shared_ptr<std::promise<std::vector<std::string> > > promise_;
    int batch_count_{0};
};

/// Batched concurrent dispatch with cancellation pipes.
class ConcurrentRunner {
public:
    static constexpr size_t MAX_CONCURRENT_RESOLVERS = 3;

    explicit ConcurrentRunner(const std::vector<std::unique_ptr<ResolverBase> > &resolvers);

    [[nodiscard]] std::expected<std::vector<std::string>, DnsErrorInfo>
    run(const std::string &host, RecordKind type) const;

private:
    const std::vector<std::unique_ptr<ResolverBase> > &resolvers_;
};

// ===========================================================================
//  SingleResolverRunner  —  implementations
// ===========================================================================

SingleResolverRunner::SingleResolverRunner(const ResolverBase &resolver) : resolver_(resolver) {
}

std::expected<std::vector<std::string>, DnsErrorInfo>
SingleResolverRunner::run(const std::string &host, RecordKind type, std::uint32_t max_retries,
                          std::uint32_t backoff_ms) const {
    unsigned actual_retries = 0;
    auto result = Utils::Retry::retry_on_error<std::vector<std::string>, DnsErrorInfo>(
        [this, &host, &type]() -> std::expected<std::vector<std::string>, DnsErrorInfo> {
            return try_resolve(resolver_, host, type, {});
        },
        max_retries, [](const DnsErrorInfo &e) { return is_retryable(e.code); },
        backoff_ms, &actual_retries);

    if (!result) {
        if (result.error().code == DnsError::NODATA) {
            SPDLOG_DEBUG(R"(DNS lookup for "{}" returned no records)", host);
        } else {
            SPDLOG_WARN(R"(DNS lookup for domain "{}" type: {} failed after {} retries. Error: {})", host,
                        magic_enum::enum_name(type), actual_retries, error_to_str(result.error().code));
        }
        return std::unexpected(std::move(result.error()));
    }

    if (result->size() > 1) {
        SPDLOG_WARN(R"(Domain "{}" resolved to more than one address (count: {}))", host, result->size());
    }

    return std::move(*result);
}

// ===========================================================================
//  FallbackRunner  —  implementations
// ===========================================================================

FallbackRunner::FallbackRunner(const std::vector<std::unique_ptr<ResolverBase> > &resolvers) : resolvers_(resolvers) {
}

std::expected<std::vector<std::string>, DnsErrorInfo>
FallbackRunner::run(const std::string &host, RecordKind type) const {
    DnsErrorInfo last_error{
        DnsError::NODATA,
        fmt::format(R"(DNS lookup for domain "{}" returned no records)", host)
    };

    // Shuffle access indices so concurrent queries spread across resolvers
    // instead of all hammering resolvers_[0] first (thundering herd avoidance).
    // Each query gets an independent random order.
    std::vector<size_t> indices(resolvers_.size());
    std::iota(indices.begin(), indices.end(), size_t{0});
    std::shuffle(indices.begin(), indices.end(), Utils::Random::engine());

    for (const auto idx: indices) {
        const auto &resolver = resolvers_[idx];
        const auto id = resolver->get_id();

        auto result = try_resolve(*resolver, host, type, {});
        if (result) {
            if (result->size() > 1) {
                SPDLOG_WARN(R"(Resolver #{} Domain "{}" resolved to more than one address (count: {}))", id, host,
                            result->size());
            }
            SPDLOG_DEBUG(R"(Fallback resolver #{} returned {} record(s) for "{}": {})", id, result->size(), host,
                         fmt::join(*result, ", "));
            return std::move(*result);
        }

        // Error path — classify by DnsErrorInfo error code.
        const auto &error = result.error();
        const auto err_code = error.code;

        SPDLOG_DEBUG(R"(Fallback resolver #{} failed for "{}": {})", id, host, error_to_str(err_code));
        last_error = error;

        switch (err_code) {
            case DnsError::NX_DOMAIN:
            case DnsError::PARSE:
            case DnsError::CONFIG:
                // Non-retryable error — stop iteration immediately.
                return std::unexpected(error);
            case DnsError::NODATA:
            case DnsError::RETRY:
            case DnsError::UNKNOWN:
            case DnsError::CONNECTION:
            case DnsError::SERVER_REFUSED:
            case DnsError::CANCELLED:
                SPDLOG_DEBUG(R"(Fallback resolver #{} returned a retryable error, moving to next)", id);
                continue;
        }
    }

    if (resolvers_.size() > 1) {
        SPDLOG_ERROR(R"(All {} fallback resolver(s) failed for domain "{}", last error: {})", resolvers_.size(),
                     host, error_to_str(last_error.code));
    }

    return std::unexpected(std::move(last_error));
}

// ===========================================================================
//  BatchRunner  —  implementations
// ===========================================================================

BatchRunner::BatchRunner(const std::string &host, RecordKind type) noexcept
    : host_(host), type_(type) {
}

std::expected<std::vector<std::string>, DnsErrorInfo>
BatchRunner::run(std::span<const std::unique_ptr<ResolverBase>> batch) {
    completed_ = 0;
    has_nxdomain_ = false;
    has_definitive_ = false;
    batch_error_ = DnsError::NODATA;

    batch_count_ = static_cast<int>(batch.size());
    promise_ = std::make_shared<std::promise<std::vector<std::string> > >();
    auto future = promise_->get_future();

    SPDLOG_DEBUG(R"(Launching batch of {} resolver(s) for "{}")", batch_count_, host_);

    {
        std::vector<std::jthread> threads;
        threads.reserve(static_cast<size_t>(batch_count_));

        for (const auto &resolver: batch) {
            threads.emplace_back(std::bind_front(&BatchRunner::resolve_one, this), std::ref(*resolver));
        }

        auto batch_result = future.get();
        if (!batch_result.empty()) {
            return batch_result;
        }
    }

    // ── All resolvers in this batch finished without a valid result ──
    if (has_nxdomain_.load()) {
        return std::unexpected(DnsErrorInfo{
            DnsError::NX_DOMAIN,
            fmt::format(R"(Domain "{}" does not exist (NXDOMAIN))", host_)
        });
    }

    if (has_definitive_.load()) {
        auto err = batch_error_.load();
        return std::unexpected(DnsErrorInfo{
            err,
            fmt::format(R"(DNS lookup for "{}" failed: {})", host_, error_to_str(err))
        });
    }

    auto err = batch_error_.load();
    return std::unexpected(DnsErrorInfo{
        err,
        fmt::format(R"(DNS lookup for "{}" returned {})", host_, error_to_str(err))
    });
}

void BatchRunner::resolve_one(const std::stop_token &st, const ResolverBase &resolver) {
    auto [read, write] = Utils::make_pipe();

    std::stop_callback cb(st, [&write] {
        if (write) [[likely]] {
            alignas(std::uint64_t) char buf[8] = {};
            [[maybe_unused]] auto _ = ::write(write.get(), buf, sizeof(buf));
        }
    });

    if (st.stop_requested()) {
        return;
    }

    Utils::CancellationToken cancel_token(read.get());
    auto result = try_resolve(resolver, host_, type_, cancel_token);
    const auto id = resolver.get_id();

    if (result) {
        SPDLOG_DEBUG(R"(Resolver #{} returned {} record(s) for "{}")", id, result->size(), host_);

        [[maybe_unused]] int prev = completed_.fetch_add(1, std::memory_order_acq_rel);
        set_promise_value(std::move(*result));
        return;
    }

    const auto &error = result.error();
    const auto err_code = error.code;

    if (err_code == DnsError::CANCELLED) {
        SPDLOG_TRACE(R"(Resolver #{} cancelled for "{}" — another resolver answered first)", id, host_);

        signal_completion(completed_.fetch_add(1, std::memory_order_acq_rel));
        return;
    }

    if (err_code == DnsError::NX_DOMAIN) {
        SPDLOG_DEBUG(R"(Resolver #{} returned NXDOMAIN for "{}")", id, host_);
        has_nxdomain_.store(true, std::memory_order_relaxed);
    } else if (err_code == DnsError::PARSE || err_code == DnsError::CONFIG) {
        SPDLOG_TRACE(R"(Resolver #{} failed for "{}": {})", id, host_, error_to_str(err_code));
        has_definitive_.store(true, std::memory_order_relaxed);
        batch_error_.store(err_code, std::memory_order_relaxed);
    } else {
        SPDLOG_TRACE(R"(Resolver #{} returned {} for "{}")", id, host_, error_to_str(err_code));
        batch_error_.store(err_code, std::memory_order_relaxed);
    }

    signal_completion(completed_.fetch_add(1, std::memory_order_acq_rel));
}

void BatchRunner::signal_completion(int prev) noexcept {
    if (prev + 1 == batch_count_) {
        set_promise_value({});
    }
}

void BatchRunner::set_promise_value(std::vector<std::string> value) noexcept {
    try {
        promise_->set_value(std::move(value));
    } catch (const std::future_error &) {
        // Another resolver already set the promise — fine.
    }
}

// ===========================================================================
//  ConcurrentRunner  —  implementations
// ===========================================================================

ConcurrentRunner::ConcurrentRunner(
    const std::vector<std::unique_ptr<ResolverBase> > &resolvers) : resolvers_(resolvers) {
}

std::expected<std::vector<std::string>, DnsErrorInfo>
ConcurrentRunner::run(const std::string &host, RecordKind type) const {
    const auto total = resolvers_.size();
    SPDLOG_DEBUG(R"(Concurrent mode: {} resolver(s) for "{}", {} per batch)", total, host, MAX_CONCURRENT_RESOLVERS);

    DnsErrorInfo last_error{
        DnsError::NODATA,
        fmt::format(R"(DNS lookup for domain "{}" returned no records)", host)
    };
    BatchRunner batch_runner(host, type);

    for (size_t offset = 0; offset < total; offset += MAX_CONCURRENT_RESOLVERS) {
        const auto batch_end = std::min(offset + MAX_CONCURRENT_RESOLVERS, total);
        auto batch = std::span(resolvers_).subspan(offset, batch_end - offset);

        auto result = batch_runner.run(batch);
        if (result && !result->empty()) {
            return std::move(*result);
        }

        if (!result) {
            const auto &err = result.error();
            // Definitive errors — stop iterating batches.
            if (err.code == DnsError::NX_DOMAIN ||
                err.code == DnsError::PARSE ||
                err.code == DnsError::CONFIG) {
                return result;
            }
            last_error = err;
        }
    }

    if (total > 1) {
        SPDLOG_ERROR(R"(All {} resolver(s) failed for domain "{}", last error: {})", total, host,
                     error_to_str(last_error.code));
    }

    return std::unexpected(std::move(last_error));
}

// ===========================================================================
//  ResolverDispatcher::Impl  —  private implementation (thin delegation)
// ===========================================================================

struct ResolverDispatcher::Impl {
    Impl(std::vector<std::unique_ptr<ResolverBase> > resolvers, Config::ResolverStrategy strategy);

    ~Impl();

    /// Resolve a hostname with retry support (single-resolver mode only).
    /// For multi-resolver mode, delegates to resolve_multi() without retry
    /// — resolver redundancy provides fault tolerance.
    /// @return  Resolved addresses on success, or a categorised error on failure.
    [[nodiscard]] std::expected<std::vector<std::string>, DnsErrorInfo>
    resolve(const std::string &host, RecordKind type, std::uint32_t max_retries, std::uint32_t backoff_ms) const;

    /// Resolve a hostname across multiple resolvers (fallback or concurrent).
    /// Dispatches to FallbackRunner or ConcurrentRunner based on the strategy.
    /// @return  Resolved addresses on success, or a categorised error on failure.
    [[nodiscard]] std::expected<std::vector<std::string>, DnsErrorInfo>
    resolve_multi(const std::string &host, RecordKind type) const;

    std::vector<std::unique_ptr<ResolverBase> > resolvers_;
    Config::ResolverStrategy strategy_{Config::ResolverStrategy::CONCURRENT};
};

// ===========================================================================
//  ResolverDispatcher::Impl  —  implementations
// ===========================================================================

ResolverDispatcher::Impl::Impl(std::vector<std::unique_ptr<ResolverBase> > resolvers, Config::ResolverStrategy strategy)
    : resolvers_(std::move(resolvers)), strategy_(strategy) {
}

ResolverDispatcher::Impl::~Impl() = default;

std::expected<std::vector<std::string>, DnsErrorInfo>
ResolverDispatcher::Impl::resolve(const std::string &host, RecordKind type, std::uint32_t max_retries,
                                  std::uint32_t backoff_ms) const {
    // Retry is only applied in single-resolver modes (exactly one resolver).
    // Multi-resolver mode (size > 1) runs without retry — the redundancy of multiple resolvers
    // provides fault tolerance, and retrying the entire multi-resolver round is not desired.
    if (resolvers_.size() == 1) {
        SingleResolverRunner runner(*resolvers_[0]);
        return runner.run(host, type, max_retries, backoff_ms);
    }

    return resolve_multi(host, type);
}

std::expected<std::vector<std::string>, DnsErrorInfo>
ResolverDispatcher::Impl::resolve_multi(const std::string &host, RecordKind type) const {
    if (strategy_ == Config::ResolverStrategy::FALLBACK) {
        SPDLOG_DEBUG(R"(Fallback mode: trying {} resolver(s) sequentially for "{}")", resolvers_.size(), host);
        FallbackRunner runner(resolvers_);
        return runner.run(host, type);
    }

    ConcurrentRunner runner(resolvers_);
    return runner.run(host, type);
}

// ===========================================================================
//  ResolverDispatcher public API — thin delegation to Impl
// ===========================================================================

ResolverDispatcher::ResolverDispatcher(std::vector<std::unique_ptr<ResolverBase> > resolvers,
                                       Config::ResolverStrategy strategy)
    : impl_(std::make_unique<Impl>(std::move(resolvers), strategy)) {
}

ResolverDispatcher::~ResolverDispatcher() = default;

ResolverDispatcher::ResolverDispatcher(ResolverDispatcher &&) noexcept = default;

ResolverDispatcher &ResolverDispatcher::operator=(ResolverDispatcher &&) noexcept = default;

std::expected<std::vector<std::string>, DnsErrorInfo>
ResolverDispatcher::resolve(const std::string &host, RecordKind type, std::uint32_t max_retries,
                            std::uint32_t backoff_ms) const {
    return impl_->resolve(host, type, max_retries, backoff_ms);
}
