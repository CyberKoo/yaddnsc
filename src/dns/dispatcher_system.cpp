//
// Created by Kotarou on 2026/6/28.
//
// ── System dispatcher (libresolv-based variant) ──
//
//    This is the default dispatcher implementation using the system's
//    libresolv (res_nquery / res_query).  It is the default because
//    YADDNSC_USE_NATIVE_DNS defaults to OFF in CMakeLists.txt.
//
//    A self-contained native dispatcher (YADDNSC_USE_NATIVE_DNS) is
//    available and recommended for musl targets (auto-detected) and
//    for environments where libresolv is not available or undesirable.
//    See dispatcher.cpp for the native variant.
//
//
//

#include "dispatcher.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <numeric>
#include <optional>
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
//  ResolverDispatcher::Impl  —  private implementation
// ===========================================================================

struct ResolverDispatcher::Impl {
    // ── Constants ──
    static constexpr size_t MAX_CONCURRENT_RESOLVERS = 3;

    // ── Types ──
    struct ConcurrentState {
        std::mutex mtx; ///< Guards result/error flags.
        std::condition_variable cv; ///< Notified when a resolver completes.

        std::vector<std::string> result; ///< Valid result from the fastest resolver.
        bool has_result = false; ///< True once a resolver returns a valid answer.

        bool has_nxdomain = false; ///< True if any resolver returned NXDOMAIN.
        std::optional<DnsErrorInfo> definitive_error; ///< Non-retryable error from any resolver.
        std::optional<DnsErrorInfo> transient_error; ///< Retryable error from any resolver.

        int completed = 0; ///< Number of resolvers that finished.
        int total = 0; ///< Total resolvers in this batch.

        Utils::CancellationSource cancel_source; ///< Cancellation pipe for this batch.
    };

    // ── Static functions ──
    [[nodiscard]] static bool is_retryable(DnsError error);

    /// Query a single resolver, parse the response, and classify the result.
    /// Handles transport errors, parse exceptions, and all RCODE values uniformly.
    [[nodiscard]] static std::expected<std::vector<std::string>, DnsErrorInfo>
    try_resolve(const ResolverBase &resolver, const std::string &host, RecordKind type,
                const Utils::CancellationToken &cancel_token);

    static void dispatch_query(const ResolverBase &resolver, const std::string &host, RecordKind type,
                               const std::shared_ptr<ConcurrentState> &state,
                               const Utils::CancellationToken &cancel_token);

    // ── Constructor / Destructor ──
    Impl(std::vector<std::unique_ptr<ResolverBase> > resolvers, Config::ResolverStrategy strategy)
        : strategy_(strategy) {
        resolvers_.reserve(resolvers.size());
        for (auto &r: resolvers) {
            resolvers_.push_back(std::move(r)); // unique_ptr → shared_ptr
        }
    }

    ~Impl() = default;

    // ── Member functions ──
    [[nodiscard]] std::expected<std::vector<std::string>, DnsErrorInfo>
    resolve(const std::string &host, RecordKind type,
            std::uint32_t max_retries, std::uint32_t backoff_ms) const;

    [[nodiscard]] std::vector<std::string> resolve_single(const std::string &host, RecordKind type,
                                                          std::uint32_t max_retries, std::uint32_t backoff_ms) const;

    [[nodiscard]] std::vector<std::string> resolve_multi(const std::string &host, RecordKind type) const;

    [[nodiscard]] std::vector<std::string> resolve_fallback(const std::string &host, RecordKind type) const;

    [[nodiscard]] std::vector<std::string> resolve_concurrent(const std::string &host, RecordKind type) const;

    // ── Data members ──
    std::vector<std::shared_ptr<ResolverBase> > resolvers_;
    Config::ResolverStrategy strategy_{Config::ResolverStrategy::CONCURRENT};
};

// ===========================================================================
//  ResolverDispatcher::Impl  —  implementations
// ===========================================================================

std::expected<std::vector<std::string>, DnsErrorInfo>
ResolverDispatcher::Impl::resolve(const std::string &host, RecordKind type, std::uint32_t max_retries,
                                  std::uint32_t backoff_ms) const {
    try {
        // Retry is only applied in single-resolver modes (exactly one resolver).
        // Multi-resolver mode (size > 1) runs without retry — the redundancy of multiple resolvers
        // provides fault tolerance, and retrying the entire multi-resolver round is not desired.
        if (resolvers_.size() == 1) {
            return resolve_single(host, type, max_retries, backoff_ms);
        }

        return resolve_multi(host, type);
    } catch (const DnsLookupException &e) {
        // Boundary translation: internal throws from resolve_fallback / resolve_concurrent
        // (NXDOMAIN, PARSE, CONFIG as definitive errors) and resolver queries are converted
        // to std::expected at this module boundary.
        return std::unexpected(DnsErrorInfo{e.get_error(), e.what()});
    } catch (const std::exception &e) {
        return std::unexpected(DnsErrorInfo{
            DnsError::UNKNOWN,
            fmt::format(R"(DNS lookup for "{}" failed: {})", host, e.what())
        });
    }
}

std::vector<std::string> ResolverDispatcher::Impl::resolve_single(const std::string &host, RecordKind type,
                                                                  std::uint32_t max_retries,
                                                                  std::uint32_t backoff_ms) const {
    unsigned actual_retries = 0;
    auto result = Utils::Retry::retry_on_error<std::vector<std::string>, DnsErrorInfo>(
        [&]() -> std::expected<std::vector<std::string>, DnsErrorInfo> {
            return try_resolve(*resolvers_[0], host, type, {});
        },
        static_cast<unsigned>(max_retries), [](const DnsErrorInfo &e) { return is_retryable(e.code); },
        static_cast<unsigned long>(backoff_ms), &actual_retries);

    if (!result) {
        // NODATA means the domain exists but has no records of the requested type —
        // return an empty result without logging a warning.
        if (result.error().code == DnsError::NODATA) {
            SPDLOG_DEBUG(R"(DNS lookup for "{}" returned no records)", host);
            return {};
        }

        SPDLOG_WARN(R"(DNS lookup for domain "{}" type: {} failed after {} retries. Error: {})", host,
                    magic_enum::enum_name(type), actual_retries, error_to_str(result.error().code));
        return {};
    }

    if (result->size() > 1) {
        SPDLOG_WARN(R"(Domain "{}" resolved to more than one address (count: {}))", host, result->size());
    }

    return std::move(*result);
}

std::expected<std::vector<std::string>, DnsErrorInfo> ResolverDispatcher::Impl::try_resolve(
    const ResolverBase &resolver,
    const std::string &host,
    RecordKind type,
    const Utils::CancellationToken &cancel_token) {
    // ── 1. Query the resolver (transport layer) ──
    auto raw = resolver.query(host, type, cancel_token);
    if (!raw) {
        return std::unexpected(std::move(raw.error()));
    }

    // ── 2. Parse the raw response ──
    DNS::FormattedResponse parsed;
    try {
        parsed = DNS::RecordParser::parse_strings(*raw, host);
    } catch (const DnsLookupException &e) {
        return std::unexpected(DnsErrorInfo{e.get_error(), e.what()});
    }

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
}

bool ResolverDispatcher::Impl::is_retryable(DnsError error) {
    return error == DnsError::RETRY || error == DnsError::UNKNOWN || error == DnsError::CONNECTION;
}

void ResolverDispatcher::Impl::dispatch_query(const ResolverBase &resolver, const std::string &host, RecordKind type,
                                              const std::shared_ptr<ConcurrentState> &state,
                                              const Utils::CancellationToken &cancel_token) {
    const auto id = resolver.get_id();
    auto result = try_resolve(resolver, host, type, cancel_token);

    if (result) {
        // Fastest resolver returned a valid result — signal success.
        std::lock_guard lock(state->mtx);

        // Another resolver may have already answered; only take the first.
        if (!state->has_result) {
            SPDLOG_DEBUG(R"(Resolver #{} returned {} record(s) for "{}")", id, result->size(), host);
            state->result = std::move(*result);
            state->has_result = true;

            state->cancel_source.trigger();

            state->cv.notify_one();
        }
        return;
    }

    // Error path — classify by DnsErrorInfo error code.
    const auto &error = result.error();
    const auto err_code = error.code;

    if (err_code == DnsError::CANCELLED) {
        SPDLOG_TRACE(R"(Resolver #{} cancelled for "{}" — another resolver answered first)", id, host);
        return;
    }

    {
        std::lock_guard lock(state->mtx);

        switch (err_code) {
            case DnsError::RETRY:
            case DnsError::UNKNOWN:
            case DnsError::CONNECTION:
                SPDLOG_TRACE(R"(Resolver #{} returned retryable error for "{}": {})", id, host, error_to_str(err_code));
                if (!state->transient_error.has_value()) {
                    state->transient_error = error;
                }
                break;

            case DnsError::NX_DOMAIN:
                SPDLOG_DEBUG(R"(Resolver #{} returned NXDOMAIN for "{}")", id, host);
                state->has_nxdomain = true;
                if (!state->definitive_error.has_value()) {
                    state->definitive_error = error;
                }
                break;

            case DnsError::SERVER_REFUSED:
                SPDLOG_TRACE(R"(Resolver #{} refused query for "{}")", id, host);
                if (!state->definitive_error.has_value() ||
                    state->definitive_error->code != DnsError::NX_DOMAIN) {
                    state->definitive_error = error;
                }
                break;

            case DnsError::NODATA:
                SPDLOG_TRACE(R"(Resolver #{} returned NODATA for "{}")", id, host);
                break;

            default:
                // PARSE, CONFIG — definitive, non-retryable errors.
                SPDLOG_TRACE(R"(Resolver #{} failed for "{}": {})", id, host, error_to_str(err_code));
                if (!state->definitive_error.has_value()) {
                    state->definitive_error = error;
                }
                break;
        }

        ++state->completed;
        state->cv.notify_one();
    }
}

std::vector<std::string> ResolverDispatcher::Impl::resolve_multi(const std::string &host, RecordKind type) const {
    if (strategy_ == Config::ResolverStrategy::FALLBACK) {
        SPDLOG_DEBUG(R"(Fallback mode: trying {} resolver(s) sequentially for "{}")", resolvers_.size(), host);
        return resolve_fallback(host, type);
    }

    return resolve_concurrent(host, type);
}

std::vector<std::string> ResolverDispatcher::Impl::resolve_fallback(const std::string &host, RecordKind type) const {
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
                throw DnsLookupException(error.message, error.code);
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
        SPDLOG_ERROR(R"(All {} fallback resolver(s) failed for domain "{}", last error: {})", resolvers_.size(), host,
                     error_to_str(last_error.code));
    }

    return {};
}

std::vector<std::string> ResolverDispatcher::Impl::resolve_concurrent(const std::string &host, RecordKind type) const {
    const auto total = resolvers_.size();
    SPDLOG_DEBUG(R"(Concurrent mode: {} resolver(s) for "{}", {} per batch)", total, host, MAX_CONCURRENT_RESOLVERS);

    DnsErrorInfo last_error{
        DnsError::NODATA,
        fmt::format(R"(DNS lookup for domain "{}" returned no records)", host)
    };

    for (size_t offset = 0; offset < total; offset += MAX_CONCURRENT_RESOLVERS) {
        const auto batch_end = std::min(offset + MAX_CONCURRENT_RESOLVERS, total);
        const auto batch_count = static_cast<int>(batch_end - offset);

        auto state = std::make_shared<ConcurrentState>();
        state->total = batch_count;

        // Create cancellation source for this batch.
        if (batch_count > 1) {
            state->cancel_source = {};  // Re-constructs with a fresh pipe.
        }

        SPDLOG_DEBUG(R"(Launching batch of {} resolver(s) ({}-{}) for "{}")", batch_count, offset, batch_end - 1, host);

        for (size_t i = offset; i < batch_end; ++i) {
            SPDLOG_TRACE(R"(Batched concurrent resolver #{} for "{}")", resolvers_[i]->get_id(), host);
            auto cancel_token = state->cancel_source.token();
            std::thread([resolver = resolvers_[i], host, type, state, cancel_token] {
                dispatch_query(*resolver, host, type, state, cancel_token);
            }).detach();
        }

        // Wait for the fastest valid result or all threads to finish.
        std::vector<std::string> batch_result;
        bool has_result = false;
        {
            std::unique_lock lock(state->mtx);
            state->cv.wait(lock, [&state] { return state->has_result || state->completed == state->total; });

            if (state->has_result) {
                has_result = true;
                batch_result = std::move(state->result);
            }
        }

        // Destroy the cancellation source for this batch (closes the pipe fds).
        state->cancel_source = {};

        // Fastest resolver returned a valid result — take it.
        if (has_result) {
            SPDLOG_DEBUG(R"(DNS lookup for "{}" returned {} record(s): {})", host, batch_result.size(),
                         fmt::join(batch_result, ", "));
            return batch_result;
        }

        // All resolvers in the batch finished without a valid result.
        if (state->has_nxdomain) {
            throw DnsLookupException(state->definitive_error->message, state->definitive_error->code);
        }

        if (state->definitive_error.has_value()) {
            last_error = *state->definitive_error;
            // REFUSED is per-resolver — continue to the next batch.
            if (!is_retryable(last_error.code) && last_error.code != DnsError::SERVER_REFUSED) {
                throw DnsLookupException(last_error.message, last_error.code);
            }
        } else if (state->transient_error.has_value()) {
            last_error = *state->transient_error;
        }
    }

    if (total > 1) {
        SPDLOG_ERROR(R"(All {} resolver(s) failed for domain "{}", last error: {})", total, host,
                     error_to_str(last_error.code));
    }

    return {};
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
