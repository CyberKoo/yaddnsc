#include "dot.h"

#include <array>
#include <new>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include <coroutine>  // IWYU pragma: keep — coroutine lowering

#include <expected>
#include <spdlog/spdlog.h>

#include "coro/cancelled.h"
#include "coro/scope.hpp"
#include "infrastructure/dns/bootstrap/bootstrap.h"
#include "infrastructure/dns/dns_packet_exception.h"
#include "infrastructure/dns/query_budget.hpp"
#include "infrastructure/dns/util.hpp"
#include "infrastructure/dns/validator.h"
#include "infrastructure/dns/wire/builder.h"
#include "infrastructure/dns/wire/framing.h"
#include "infrastructure/network/factory/default_stream_factory.h"
#include "infrastructure/network/transport/stream.h"
#include "support/fmt.hpp"
#include "yaddnsc/util/format.hpp"

namespace dns {
namespace {
constexpr unsigned char ALPN_DOT[] = {3, 'd', 'o', 't'};
constexpr std::size_t PAD_BLOCK = 128;
constexpr std::size_t EDNS_PAD_OVERHEAD = 15;

[[nodiscard]] std::vector<std::uint8_t> build_padded_query(const std::string& host, const RecordType type) {
    const auto base = QueryBuilder{}.add_question(host, type).build();
    const auto raw_size = base.size() + EDNS_PAD_OVERHEAD;
    const auto pad_length = raw_size % PAD_BLOCK == 0 ? PAD_BLOCK : PAD_BLOCK - raw_size % PAD_BLOCK;
    const EdnsOption padding{12, std::vector<std::uint8_t>(pad_length)};
    return QueryBuilder{}.add_question(host, type).add_edns(512, 0, false, std::span(&padding, 1)).build();
}

[[nodiscard]] coro::Task<std::expected<void, domain::DnsErrorInfo>> connect_with_budget(net::Stream& stream,
                                                                                        const Policy& policy) {
    auto connected =
        co_await coro::with_timeout(policy.connect_budget, [&stream]() { return stream.ensure_connected(); });
    if (connected.timed_out) {
        co_return std::unexpected(domain::DnsErrorInfo{domain::DnsError::RETRY, "DoT connection timed out"});
    }
    if (!*connected) {
        co_return std::unexpected(domain::DnsErrorInfo{domain::DnsError::CONNECTION, "DoT connection failed"});
    }
    co_return {};
}

/// One framed exchange on an owned, guarded stream; no retry or recovery here.
[[nodiscard]] coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> exchange_dns(
    net::Stream& stream, const std::span<const std::uint8_t> query, const std::span<const std::uint8_t> framed) {
    if (auto sent = co_await stream.send_all(framed); !sent) {
        co_return std::unexpected(domain::DnsErrorInfo{domain::DnsError::CONNECTION, "DoT send failed"});
    }
    std::array<std::uint8_t, 2> prefix{};
    if (auto received = co_await stream.read_exact(prefix); !received) {
        co_return std::unexpected(domain::DnsErrorInfo{domain::DnsError::CONNECTION, "DoT receive failed"});
    }
    const auto length = read_length(prefix);
    if (!length) {
        co_return std::unexpected(domain::DnsErrorInfo{domain::DnsError::PARSE,
                                                       "DoT server announced an invalid or oversized response length"});
    }
    std::vector<std::uint8_t> response(*length);
    if (auto received = co_await stream.read_exact(response); !received) {
        co_return std::unexpected(domain::DnsErrorInfo{domain::DnsError::CONNECTION, "DoT receive failed"});
    }
    if (auto valid = Validator::validate_response(query, response); !valid) {
        co_return std::unexpected(std::move(valid.error()));
    }
    if (detail::is_truncated(response)) {
        co_return std::unexpected(
            domain::DnsErrorInfo{domain::DnsError::PARSE, "DoT server returned a truncated (TC) answer"});
    }
    co_return response;
}
}  // namespace

DotResolver::DotResolver(std::string host, const std::uint16_t port, EndpointOptions options)
    : host_(std::move(host)), port_(port), options_(std::move(options)) {
    if (host_.empty() || host_.find('\0') != std::string::npos || port_ == 0) {
        throw std::invalid_argument("DoT endpoint requires a host and a nonzero port");
    }
    options_.tls.alpn_proto = ALPN_DOT;
    if (!options_.tls.sni_hostname && !domain::InetAddress::parse(host_))
        options_.tls.sni_hostname = host_;
}

DotResolver::~DotResolver() {
    drop_stream();
}

coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> DotResolver::query(
    std::string host, const domain::RecordKind kind) {
    auto guard = co_await mutex_.lock();
    co_return co_await detail::bounded_query([this, &host, kind]() { return query_once(host, kind); }, options_.policy);
}

coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> DotResolver::query_once(
    std::string host, const domain::RecordKind kind) {
    try {
        if (close_requested_) {
            close_requested_ = false;
            drop_stream();
        }
        const auto query_bytes = build_padded_query(host, Util::type_to_record_type(kind));
        const auto framed = frame_message(query_bytes);
        if (!framed) {
            co_return std::unexpected(
                domain::DnsErrorInfo{domain::DnsError::INVALID_QUERY, "DoT query exceeds the framing limit"});
        }
        const bool reused = stream_ != nullptr && stream_->connected();
        if (auto ready = co_await ensure_stream(); !ready) {
            drop_stream();
            co_return std::unexpected(std::move(ready.error()));
        }
        auto response = co_await exchange_dns(*stream_, query_bytes, *framed);
        if (!response) {
            const bool repair = reused && response.error().code == domain::DnsError::CONNECTION;
            drop_stream();
            if (!repair)
                co_return std::unexpected(std::move(response.error()));
            // Repair only a lost old session, within the same attempt scope.
            if (auto ready = co_await ensure_stream(); !ready) {
                co_return std::unexpected(std::move(ready.error()));
            }
            response = co_await exchange_dns(*stream_, query_bytes, *framed);
            if (!response) {
                drop_stream();
                co_return std::unexpected(std::move(response.error()));
            }
        }
        if (close_requested_) {
            close_requested_ = false;
            drop_stream();
        }
        SPDLOG_TRACE(R"(Resolver #{} query succeeded ({} bytes) for "{}")", id(), response->size(), host);
        co_return std::move(*response);
    } catch (const coro::Cancelled&) {
        drop_stream();
        throw;
    } catch (const std::bad_alloc&) {
        drop_stream();
        throw;
    } catch (const DnsPacketException& error) {
        co_return std::unexpected(
            domain::DnsErrorInfo{domain::DnsError::INVALID_QUERY,
                                 fmt::format(R"(Failed to build a DNS query for "{}": {})", host, error.what())});
    } catch (...) {
        drop_stream();
        throw;
    }
}

coro::Task<std::expected<void, domain::DnsErrorInfo>> DotResolver::ensure_stream() {
    if (stream_ && !stream_->connected())
        drop_stream();
    if (stream_)
        co_return {};
    auto resolved = options_.resolve
                        ? co_await options_.resolve(host_, std::nullopt)
                        : co_await bootstrap_resolve(host_, std::nullopt, options_.bootstrap_dns, options_.policy);
    if (!resolved) {
        co_return std::unexpected(domain::DnsErrorInfo{
            domain::DnsError::CONNECTION,
            fmt::format(R"(DoT endpoint "{}" resolution failed: {})", host_, resolved.error().message)});
    }
    net::DefaultStreamFactory fallback;
    net::StreamFactory& factory = options_.factory ? *options_.factory : fallback;
    domain::DnsErrorInfo last{domain::DnsError::CONNECTION, "No DoT address"};
    for (const auto& address : *resolved) {
        auto stream = factory.create_tls(address, port_, options_.connect, options_.tls, options_.tls_context);
        auto connected = co_await connect_with_budget(*stream, options_.policy);
        if (connected) {
            stream_ = std::move(stream);
            co_return {};
        }
        last = std::move(connected.error());
    }
    co_return std::unexpected(std::move(last));
}

void DotResolver::close() noexcept {
    if (mutex_.locked()) {
        close_requested_ = true;
        return;
    }
    drop_stream();
}

void DotResolver::drop_stream() noexcept {
    if (stream_) {
        stream_->close();
        stream_.reset();
    }
}
}  // namespace dns
