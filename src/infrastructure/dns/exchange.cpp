#include "exchange.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <coroutine>  // IWYU pragma: keep — coroutine lowering

#include <expected>

#include "coro/scope.hpp"
#include "coro/sleep.hpp"
#include "domain/error/dns_error.h"
#include "infrastructure/dns/dns_lookup_exception.h"
#include "infrastructure/dns/parser.h"
#include "infrastructure/dns/query_budget.hpp"
#include "infrastructure/dns/validator.h"
#include "infrastructure/dns/wire/framing.h"
#include "infrastructure/network/transport/datagram.h"
#include "infrastructure/network/transport/tcp_stream.h"
#include "infrastructure/network/transport/udp_socket.h"
#include "support/fmt.hpp"
#include "yaddnsc/util/format.hpp"

namespace dns::detail {
namespace {
[[nodiscard]] domain::DnsErrorInfo connection_error(const char* stage, const domain::InetAddress& server,
                                                    const std::uint16_t port) {
    return {domain::DnsError::CONNECTION, fmt::format("DNS {} to {}:{} failed", stage, server.to_string(), port)};
}

[[nodiscard]] domain::DnsErrorInfo budget_exceeded(const char* stage, const domain::InetAddress& server,
                                                   const std::uint16_t port) {
    return {domain::DnsError::RETRY,
            fmt::format("DNS {} to {}:{} timed out after {}ms", stage, server.to_string(), port,
                        std::chrono::duration_cast<std::chrono::milliseconds>(WIRE_BUDGET).count())};
}

[[nodiscard]] coro::Task<std::expected<UdpAnswer, domain::DnsErrorInfo>> exchange_udp(
    const domain::InetAddress server, const std::uint16_t port, const std::span<const std::uint8_t> query,
    const Question& asked) {
    net::UdpSocket socket{server.get_family()};
    if (auto sent = co_await socket.send_to(server, port, query); !sent) {
        co_return std::unexpected(connection_error("UDP send", server, port));
    }
    std::size_t discarded = 0;
    for (;;) {
        // Yield periodically under a stray-packet flood, without accepting it
        // or resetting the timer. A foreign sender cannot force early failure.
        if (discarded != 0 && discarded % 32 == 0)
            co_await coro::sleep_for(std::chrono::milliseconds(1));
        ++discarded;
        std::array<std::uint8_t, dns::MAX_MESSAGE_SIZE> buffer{};
        auto received = co_await socket.recv_from(buffer);
        if (!received)
            co_return std::unexpected(connection_error("UDP receive", server, port));
        if (received->from != server || received->port != port)
            continue;
        const auto bytes = std::span(buffer).first(received->size);
        // Match before accepting even kernel-truncated packets. Stray replies
        // cannot terminate the query or trigger a TCP connection; the same
        // timeout keeps running while they are discarded. `asked` was parsed
        // from `query` before the first send, so a datagram costs one question
        // parse — its own — instead of three.
        auto matched = Validator::match_response(query, asked, bytes);
        if (!matched)
            co_return std::unexpected(std::move(matched.error()));
        if (!*matched)
            continue;
        co_return UdpAnswer{.bytes = {bytes.begin(), bytes.end()}, .truncated = received->truncated};
    }
}

[[nodiscard]] coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> exchange_tcp(
    const domain::InetAddress server, const std::uint16_t port, const std::span<const std::uint8_t> query) {
    const auto framed = dns::frame_message(query);
    if (!framed) {
        co_return std::unexpected(
            domain::DnsErrorInfo{domain::DnsError::INVALID_QUERY, "DNS query exceeds the classic TCP limit"});
    }
    net::TcpStream stream{server, port};
    if (auto connected = co_await stream.ensure_connected(); !connected) {
        co_return std::unexpected(connection_error("TCP connect", server, port));
    }
    if (auto sent = co_await stream.send_all(*framed); !sent) {
        co_return std::unexpected(connection_error("TCP send", server, port));
    }
    std::array<std::uint8_t, 2> prefix{};
    if (auto received = co_await stream.read_exact(prefix); !received) {
        co_return std::unexpected(connection_error("TCP receive", server, port));
    }
    const auto length = dns::read_length(prefix);
    if (!length) {
        co_return std::unexpected(domain::DnsErrorInfo{
            domain::DnsError::PARSE, "DNS server announced an invalid or oversized TCP message length"});
    }
    std::vector<std::uint8_t> response(*length);
    if (auto received = co_await stream.read_exact(response); !received) {
        co_return std::unexpected(connection_error("TCP receive", server, port));
    }
    co_return response;
}

// TCP is an implementation detail of classic fallback, not a public exchange.
coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> query_tcp(
    const domain::InetAddress server, const std::uint16_t port, const std::span<const std::uint8_t> query) {
    auto outcome =
        co_await coro::with_timeout(WIRE_BUDGET, [server, port, query]() { return exchange_tcp(server, port, query); });
    if (outcome.timed_out)
        co_return std::unexpected(budget_exceeded("TCP query", server, port));
    co_return std::move(*outcome);
}
}  // namespace

coro::Task<std::expected<UdpAnswer, domain::DnsErrorInfo>> query_udp(const domain::InetAddress server,
                                                                     const std::uint16_t port,
                                                                     const std::span<const std::uint8_t> query) {
    if (auto valid = Validator::validate_query(query); !valid)
        co_return std::unexpected(std::move(valid.error()));
    // The request never changes while the receive loop runs, so its question is
    // parsed once here and every datagram is matched against that copy.
    Question asked;
    try {
        asked = RecordParser::parse_question(query);
    } catch (const DnsLookupException& error) {
        co_return std::unexpected(domain::DnsErrorInfo{domain::DnsError::INVALID_QUERY, error.what()});
    }
    auto outcome = co_await coro::with_timeout(
        WIRE_BUDGET, [server, port, query, &asked]() { return exchange_udp(server, port, query, asked); });
    if (outcome.timed_out)
        co_return std::unexpected(budget_exceeded("UDP query", server, port));
    co_return std::move(*outcome);
}

coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> query_classic(
    const domain::InetAddress server, const std::uint16_t port, const std::span<const std::uint8_t> query) {
    bool tcp_started = false;
    auto outcome = co_await coro::with_timeout(
        ATTEMPT_BUDGET,
        [server, port, query,
         &tcp_started]() -> coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> {
            auto udp = co_await query_udp(server, port, query);
            if (!udp)
                co_return std::unexpected(std::move(udp.error()));
            if (!udp->truncated && !is_truncated(udp->bytes))
                co_return std::move(udp->bytes);
            tcp_started = true;
            auto tcp = co_await query_tcp(server, port, query);
            if (!tcp)
                co_return std::unexpected(std::move(tcp.error()));
            if (auto valid = Validator::validate_response(query, *tcp); !valid) {
                co_return std::unexpected(std::move(valid.error()));
            }
            if (is_truncated(*tcp)) {
                co_return std::unexpected(
                    domain::DnsErrorInfo{domain::DnsError::PARSE, "DNS server returned a truncated TCP response"});
            }
            co_return std::move(*tcp);
        });
    if (outcome.timed_out) {
        co_return std::unexpected(domain::DnsErrorInfo{
            domain::DnsError::RETRY,
            fmt::format("DNS {} to {}:{} exhausted the {}ms attempt budget", tcp_started ? "TCP fallback" : "UDP query",
                        server.to_string(), port, ATTEMPT_BUDGET.count())});
    }
    co_return std::move(*outcome);
}
}  // namespace dns::detail
