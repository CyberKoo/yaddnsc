//
// dns — DNS over TLS.
//

#include "dot.h"

#include <spdlog/spdlog.h>
#include <coroutine>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <optional>

#include "coro/scope.hpp"
#include "infrastructure/dns/bootstrap/bootstrap.h"
#include "infrastructure/dns/dns_lookup_exception.h"
#include "infrastructure/dns/dns_packet_exception.h"
#include "infrastructure/dns/util.hpp"
#include "infrastructure/dns/validator.h"
#include "infrastructure/dns/wire/builder.h"
#include "infrastructure/dns/wire/framing.h"
#include "infrastructure/network/factory/default_stream_factory.h"
#include "support/fmt.hpp"
#include "domain/error/dns_error.h"
#include "domain/network/inet_address.h"
#include "coro/cancelled.h"
#include "infrastructure/dns/types.h"
#include "infrastructure/network/transport/stream.h"
#include "yaddnsc/util/format.hpp"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not

namespace net {
enum class IoError;
}  // namespace net

namespace dns {
namespace {

/// ALPN protocol identifier "dot". The IANA ALPN Protocol IDs registry
/// registers it under RFC 7858, whose text never discusses ALPN, so there is
/// no section of it to point at.
constexpr unsigned char ALPN_DOT[] = {3, 'd', 'o', 't'};

/// Budget for one connection-establishment phase — the name resolution, or one
/// address's connect + TLS handshake — restored from the legacy
/// TLS_CONNECT_TIMEOUT: a black-holed phase fails as RETRY in about a second
/// instead of parking until the caller's scope fires. Each phase and each
/// address gets its own budget, so a slow phase never eats the next one's.
constexpr auto CONNECT_BUDGET = std::chrono::seconds(1);

/// Post-connect budget for one send/read operation. An expiry is a broken
/// session, mapped to CONNECTION (the legacy "Failed to read from ..." path),
/// not to RETRY. 1s keeps one operation well under the caller's DNS read
/// budget (DNS_READ_BUDGET, 4s) — but only per phase: an endpoint with several
/// addresses spends one connect budget per address plus the resolve and I/O
/// budgets, so their sum can exceed that 4s and the caller's scope
/// cancellation becomes the effective bound.
constexpr auto IO_BUDGET = std::chrono::seconds(1);

/// RFC 8467 §4.1 query block length for a DoT query.
constexpr std::size_t PAD_BLOCK = 128;

/// EDNS0 padding-option overhead excluding the padding bytes themselves:
/// OPT NAME(1) + TYPE(2) + CLASS(2) + TTL(4) + RDLENGTH(2) + option code(2) +
/// option length(2).
constexpr std::size_t EDNS_PAD_OVERHEAD = 15;

/// Build a query padded to the next 128-octet block (RFC 7830 option, RFC 8467
/// §4.1 block length; the TCP length prefix is excluded from the block per
/// RFC 8467 §3).
[[nodiscard]] std::vector<std::uint8_t> build_padded_query(const std::string& host, const dns::RecordType type) {
    const auto base = dns::QueryBuilder{}.add_question(host, type).build();

    const std::size_t raw_size = base.size() + EDNS_PAD_OVERHEAD;
    const std::size_t pad_length = raw_size % PAD_BLOCK == 0 ? PAD_BLOCK : PAD_BLOCK - raw_size % PAD_BLOCK;

    // RFC 7830 §3: the padding octets SHOULD be set to 0x00.
    std::vector<std::uint8_t> padding(pad_length);
    const dns::EdnsOption padding_option{12, padding};  // option 12 = padding

    return dns::QueryBuilder{}.add_question(host, type).add_edns(512, 0, false, std::span(&padding_option, 1)).build();
}

/// Connect one stream under CONNECT_BUDGET. An expired budget is the legacy
/// connect timeout and maps to RETRY; outer cancellation throws coro::Cancelled.
[[nodiscard]] coro::Task<std::expected<void, domain::DnsErrorInfo>> connect_with_budget(net::Stream& stream) {
    auto connected = co_await coro::with_timeout(
        CONNECT_BUDGET,
        [&stream]() -> coro::Task<std::expected<void, net::IoError>> { co_return co_await stream.ensure_connected(); });
    if (connected.timed_out) {
        co_return std::unexpected(domain::DnsErrorInfo{domain::DnsError::RETRY, "DoT connection timed out"});
    }
    if (!*connected) {
        co_return std::unexpected(domain::DnsErrorInfo{domain::DnsError::CONNECTION, "DoT connection failed"});
    }
    co_return {};
}

/// Endpoint bootstrap failures answer about the endpoint name, not about the
/// queried domain: NXDOMAIN or NODATA for the DoT host only means this host is
/// unknown here, never an authoritative verdict on the caller's host. Reported
/// as-is they would end the dispatcher's search instead of failing over, so
/// they are rewritten to the retryable CONNECTION, like a DoH endpoint whose
/// own resolution fails (map_http_error).
[[nodiscard]] domain::DnsErrorInfo map_resolve_error(const std::string& host, domain::DnsErrorInfo error) {
    if (error.code == domain::DnsError::NX_DOMAIN || error.code == domain::DnsError::NODATA) {
        return domain::DnsErrorInfo{domain::DnsError::CONNECTION,
                                    fmt::format(R"(DoT endpoint "{}" resolution failed: {})", host, error.message)};
    }
    return error;
}

}  // namespace

DotResolver::DotResolver(std::string host, const std::uint16_t port, EndpointOptions options)
    : host_(std::move(host)), port_(port), options_(std::move(options)) {
    options_.tls.alpn_proto = ALPN_DOT;
    // A named endpoint host is the default TLS identity (SNI and certificate
    // verification) unless the caller pinned a name. An IP-literal endpoint is
    // never copied: RFC 6066 §3 forbids an IP literal in SNI, and verification
    // then targets the connection IP by default.
    if (!options_.tls.sni_hostname.has_value() && !domain::InetAddress::parse(host_).has_value()) {
        options_.tls.sni_hostname = host_;
    }
}

DotResolver::~DotResolver() {
    close();
}

coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> DotResolver::query(
    std::string host, const domain::RecordKind kind) {
    // Waiting semantics: a concurrent query queues on the session, it is not
    // refused.
    auto guard = co_await mutex_.lock();

    try {
        const auto record_type = dns::Util::type_to_record_type(kind);
        SPDLOG_TRACE(R"(Resolver #{} lookup for domain "{}" (type {}))", id(), host,
                     static_cast<std::uint16_t>(record_type));

        const auto query_bytes = build_padded_query(host, record_type);
        const auto framed = dns::frame_message(query_bytes);
        if (!framed) {
            co_return std::unexpected(
                domain::DnsErrorInfo{domain::DnsError::PARSE, "DoT query exceeds the framing limit"});
        }

        for (int attempt = 0; attempt < 2; ++attempt) {
            if (attempt == 1) {
                // No prebuilt label: the arguments are formatted by spdlog only
                // when DEBUG is enabled, and only this branch records them.
                SPDLOG_DEBUG(R"(Connection to {}:{} failed, reconnecting)", host_, port_);
                close();  // rebuild once on a transient failure
            }
            if (auto ready = co_await ensure_stream(); !ready) {
                if (attempt == 0) {
                    continue;
                }
                co_return std::unexpected(std::move(ready.error()));
            }

            auto sent = co_await coro::with_timeout(IO_BUDGET,
                                                    [this, &framed]() -> coro::Task<std::expected<void, net::IoError>> {
                                                        co_return co_await stream_->send_all(*framed);
                                                    });
            if (sent.timed_out) {
                // Own budget fired; ancestor cancellation propagates:
                // a broken session, retried once like any transient failure.
                close();
                if (attempt == 0) {
                    continue;
                }
                co_return std::unexpected(domain::DnsErrorInfo{domain::DnsError::CONNECTION, "DoT send timed out"});
            }
            if (!*sent) {
                close();

                if (attempt == 0) {
                    continue;
                }
                co_return std::unexpected(domain::DnsErrorInfo{domain::DnsError::CONNECTION, "DoT send failed"});
            }

            std::array<std::uint8_t, 2> prefix{};
            auto got_prefix = co_await coro::with_timeout(
                IO_BUDGET, [this, &prefix]() -> coro::Task<std::expected<void, net::IoError>> {
                    co_return co_await stream_->read_exact(prefix);
                });
            if (got_prefix.timed_out) {
                close();
                if (attempt == 0) {
                    continue;
                }
                co_return std::unexpected(domain::DnsErrorInfo{domain::DnsError::CONNECTION, "DoT receive timed out"});
            }
            if (!*got_prefix) {
                close();

                if (attempt == 0) {
                    continue;
                }
                co_return std::unexpected(domain::DnsErrorInfo{domain::DnsError::CONNECTION, "DoT receive failed"});
            }
            // The two-byte prefix bounds the response to 65535 octets
            // (RFC 1035 §4.2.2), so only a zero length is invalid.
            const auto length = dns::read_length(prefix);
            if (!length) {
                close();
                co_return std::unexpected(
                    domain::DnsErrorInfo{domain::DnsError::PARSE, "DoT server announced a zero-length response"});
            }

            std::vector<std::uint8_t> response(*length);
            auto got_body = co_await coro::with_timeout(
                IO_BUDGET, [this, &response]() -> coro::Task<std::expected<void, net::IoError>> {
                    co_return co_await stream_->read_exact(response);
                });
            if (got_body.timed_out) {
                close();
                if (attempt == 0) {
                    continue;
                }
                co_return std::unexpected(domain::DnsErrorInfo{domain::DnsError::CONNECTION, "DoT receive timed out"});
            }
            if (!*got_body) {
                close();

                if (attempt == 0) {
                    continue;
                }
                co_return std::unexpected(domain::DnsErrorInfo{domain::DnsError::CONNECTION, "DoT receive failed"});
            }

            if (auto valid = dns::Validator::validate_response(query_bytes, response); !valid) {
                co_return std::unexpected(std::move(valid.error()));
            }
            SPDLOG_TRACE(R"(Resolver #{} query succeeded ({} bytes) for "{}")", id(), response.size(), host);
            co_return response;
        }
        co_return std::unexpected(domain::DnsErrorInfo{domain::DnsError::CONNECTION, "DoT query failed"});
    } catch (const coro::Cancelled&) {
        close();
        throw;
    } catch (const std::bad_alloc&) {
        throw;  // allocation failures are never downgraded to a retryable error
    } catch (const DnsLookupException& error) {
        co_return std::unexpected(domain::DnsErrorInfo{error.get_error(), error.what()});
    } catch (const DnsPacketException& error) {
        // Packet construction failure is a definitive input defect (e.g. an
        // over-long name), never a transient transport error (see ClassicResolver).
        co_return std::unexpected(domain::DnsErrorInfo{
            domain::DnsError::PARSE, fmt::format(R"(Failed to build a DNS query for "{}": {})", host, error.what())});
    } catch (const std::exception& error) {
        // Nothing in this path is expected to throw, so log the exception that
        // was not handled by a catch above before it is downgraded.
        SPDLOG_WARN(R"(Resolver #{} DoT query for "{}" raised an unexpected exception: {})", id(), host, error.what());
        // See ClassicResolver: unexpected exceptions stay retryable as UNKNOWN.
        co_return std::unexpected(domain::DnsErrorInfo{
            domain::DnsError::UNKNOWN, fmt::format(R"(DoT query for "{}" failed: {})", host, error.what())});
    }
}

coro::Task<std::expected<void, domain::DnsErrorInfo>> DotResolver::ensure_stream() {
    if (stream_ != nullptr) {
        co_return co_await connect_with_budget(*stream_);
    }

    // Name resolution gets its own CONNECT_BUDGET: with k bootstrap servers an
    // unbounded resolve could otherwise take k × UDP budget before the first
    // connect attempt even starts. It stays separate from the per-address
    // connect budgets below — a slow resolve must not eat the connect/TLS
    // handshake budget, and one black-holed address must not eat the next
    // address's.
    auto resolved = co_await coro::with_timeout(
        CONNECT_BUDGET, [this]() -> coro::Task<std::expected<std::vector<domain::InetAddress>, domain::DnsErrorInfo>> {
            if (options_.resolve) {
                co_return co_await options_.resolve(host_, std::nullopt);
            }
            co_return co_await bootstrap_resolve(host_, std::nullopt, options_.bootstrap_dns);
        });
    if (resolved.timed_out) {
        co_return std::unexpected(domain::DnsErrorInfo{domain::DnsError::RETRY, "DoT name resolution timed out"});
    }
    if (!*resolved) {
        co_return std::unexpected(map_resolve_error(host_, std::move(resolved->error())));
    }

    net::DefaultStreamFactory fallback;
    net::StreamFactory& factory = options_.factory != nullptr ? *options_.factory : fallback;

    domain::DnsErrorInfo last{domain::DnsError::CONNECTION, "no DoT address"};
    for (const domain::InetAddress& address : **resolved) {
        auto stream = factory.create_tls(address, port_, options_.connect, options_.tls, options_.tls_context);
        auto connected = co_await connect_with_budget(*stream);
        if (connected) {
            stream_ = std::move(stream);
            co_return {};
        }
        last = std::move(connected.error());
    }
    co_return std::unexpected(std::move(last));
}

void DotResolver::close() noexcept {
    if (stream_ != nullptr) {
        stream_->close();
        stream_.reset();
    }
}

}  // namespace dns
