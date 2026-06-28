//
// Created by Kotarou on 2022/4/5.
//
#include "dns.h"

#include <arpa/nameser.h>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

#include <spdlog/spdlog.h>

#include "fmt.hpp"
#include "dns_record_parser.h"
#include "dns_resolver.h"
#include "exception/dns_lookup_exception.h"
#include "resolver_base.h"

namespace {
    bool should_fallback(dns_error error) {
        return error == dns_error::RETRY || error == dns_error::UNKNOWN;
    }

    // ── Shared state for concurrent queries ──
    struct ConcurrentState {
        std::mutex mtx;
        std::condition_variable cv;

        std::vector<std::string> result;
        bool has_result = false;

        bool has_nxdomain = false;
        std::optional<DnsLookupException> definitive_error; // NXDOMAIN stored here too
        std::optional<DnsLookupException> transient_error;

        int completed = 0;
        int total = 0;
    };

    // ── Parse raw DNS response into string records ──
    std::vector<std::string>
    parse_response(const std::vector<uint8_t> &raw, [[maybe_unused]] const std::string &host) {
        const DnsRecordParser parser(raw.data(), raw.size());
        std::vector<std::string> result;
        result.reserve(parser.record_count());

        for (size_t i = 0; i < parser.record_count(); ++i) {
            auto record = parser.parse_record(i);
            SPDLOG_TRACE(R"(DNS answer #{} for "{}": {})", i, host, record);
            result.push_back(std::move(record));
        }

        return result;
    }

    // ── Run one resolver and update shared state ──
    void query_resolver(const ResolverBase &resolver, const std::string &host, dns_type type, int resolver_id,
                        const std::shared_ptr<ConcurrentState> &state) {
        try {
            auto raw_response = resolver.query(host, type);
            auto records = parse_response(raw_response, host);

            std::lock_guard lock(state->mtx);
            if (!state->has_result && !state->has_nxdomain && !records.empty()) {
                SPDLOG_DEBUG(R"(Resolver #{} returned {} record(s) for "{}")", resolver_id, records.size(), host);
                state->result = std::move(records);
                state->has_result = true;
                state->cv.notify_one();
            }
        } catch (const DnsLookupException &e) {
            std::lock_guard lock(state->mtx);

            // NXDOMAIN is definitive — abort immediately.
            if (e.get_error() == dns_error::NX_DOMAIN) {
                SPDLOG_DEBUG(R"(Resolver #{} returned NXDOMAIN for "{}")", resolver_id, host);
                state->has_nxdomain = true;
                state->definitive_error = e;
                state->cv.notify_one();
                return; // don't increment completed
            }

            SPDLOG_TRACE(R"(Resolver #{} failed for "{}": {})", resolver_id, host, DNS::error_to_str(e.get_error()));

            if (should_fallback(e.get_error())) {
                state->transient_error = e;
            } else if (!state->definitive_error.has_value()) {
                state->definitive_error = e;
            }
        } catch (...) {
            // Catch-all: any unexpected exception (e.g. std::bad_alloc) from a
            // detached thread must not propagate — that would call std::terminate().
            {
                std::lock_guard lock(state->mtx);
                SPDLOG_TRACE(R"(Resolver #{} threw an unknown exception for "{}")", resolver_id, host);
                if (!state->definitive_error.has_value()) {
                    state->definitive_error = DnsLookupException(
                        fmt::format(R"(Resolver #{} threw an unknown exception for "{}")", resolver_id, host),
                        dns_error::UNKNOWN);
                }
            }
        }

        // Signal completion: any resolver that finishes (success with no result,
        // or non-fatal failure) increments the counter and notifies.
        {
            std::lock_guard lock(state->mtx);
            ++state->completed;
            state->cv.notify_one();
        }
    }
} // anonymous namespace

std::vector<std::string>
DNS::resolve(const std::string &host, dns_type type, const std::vector<std::shared_ptr<ResolverBase> > &resolvers) {
    // ── No custom resolvers → default system resolver ─────────────────────
    if (resolvers.empty()) {
        SPDLOG_TRACE(R"(Using default system resolver for "{}")", host);
        const DnsResolver resolver;
        auto raw_response = resolver.query(host, type);
        auto result = parse_response(raw_response, host);

        if (!result.empty()) {
            SPDLOG_DEBUG(R"(DNS lookup for "{}" returned {} record(s): {})",
                         host, result.size(), fmt::join(result, ", ")
            );
        } else {
            SPDLOG_DEBUG(R"(DNS lookup for "{}" returned no records)", host);
        }
        return result;
    }

    // ── Single resolver — no concurrency overhead needed. ─────────────────
    if (resolvers.size() == 1) {
        SPDLOG_DEBUG(R"(Using single resolver for "{}")", host);
        auto raw_response = resolvers[0]->query(host, type);
        auto result = parse_response(raw_response, host);

        if (!result.empty()) {
            SPDLOG_DEBUG(R"(DNS lookup for "{}" returned {} record(s): {})",
                         host, result.size(), fmt::join(result, ", "));
        } else {
            SPDLOG_DEBUG(R"(DNS lookup for "{}" returned no records)", host);
        }
        return result;
    }

    // ── Multiple resolvers — fire all concurrently, take fastest. ─────────
    SPDLOG_DEBUG(R"(Firing {} resolver(s) concurrently for "{}")", resolvers.size(), host);

    auto state = std::make_shared<ConcurrentState>();
    state->total = static_cast<int>(resolvers.size());

    for (size_t i = 0; i < resolvers.size(); ++i) {
        SPDLOG_TRACE(R"(Launching concurrent resolver #{} for "{}")", i, host);
        // Capture resolver by shared_ptr — keeps the object alive even after
        // DNS::resolve() returns and the caller potentially destroys its copy.
        std::thread([resolver = resolvers[i], host, type, state, i] {
            query_resolver(*resolver, host, type, static_cast<int>(i), state);
        }).detach();
    }

    // Wait for: a valid result, an NXDOMAIN, or all resolvers to finish.
    {
        std::unique_lock lock(state->mtx);
        state->cv.wait(lock, [&state] {
            return state->has_result || state->has_nxdomain || state->completed == state->total;
        });
    }

    // Fast path: got a valid result.
    if (state->has_result) {
        SPDLOG_DEBUG(R"(DNS lookup for "{}" returned {} record(s): {})", host, state->result.size(),
                     fmt::join(state->result, ", "));
        return std::move(state->result);
    }

    // NXDOMAIN from any resolver is definitive.
    if (state->has_nxdomain) {
        throw *state->definitive_error;
    }

    // All resolvers finished without a result — throw the best error.
    if (state->definitive_error.has_value()) {
        throw state->definitive_error.value();
    }

    auto last_err = state->transient_error.value_or(
        DnsLookupException(
            fmt::format(R"(DNS lookup for domain "{}" returned no records)", host),
            dns_error::NODATA));

    if (state->total > 1) {
        SPDLOG_ERROR(R"(All {} resolver(s) failed for domain "{}", last error: {})",
                     state->total, host, DNS::error_to_str(last_err.get_error()));
    }

    throw last_err;
}

address_family DNS::dns2ip(dns_type type) {
    switch (type) {
        case dns_type::A:
            return address_family::IPV4;
        case dns_type::AAAA:
            return address_family::IPV6;
        default:
            return address_family::UNSPECIFIED;
    }
}

int DNS::to_ns_type(dns_type type) noexcept {
    switch (type) {
        case dns_type::A: return ns_t_a;
        case dns_type::AAAA: return ns_t_aaaa;
        case dns_type::TXT: return ns_t_txt;
        case dns_type::SOA: return ns_t_soa;
        default: return ns_t_invalid;
    }
}

std::string_view DNS::error_to_str(dns_error error) {
    switch (error) {
        case dns_error::NX_DOMAIN:
            return "no such domain (NXDOMAIN)";
        case dns_error::RETRY:
            return "retry (TRY_AGAIN)";
        case dns_error::NODATA:
            return "no data (NO_DATA)";
        case dns_error::PARSE:
            return "DNS record parse error (PARSE)";
        default:
            return "unknown DNS error";
    }
}
