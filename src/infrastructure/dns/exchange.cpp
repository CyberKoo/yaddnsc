//
// dns — shared classic wire exchanges over the coroutine transport.
//

#include "exchange.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "domain/dns/record_kind.h"
#include "infrastructure/coro/scope.hpp"
#include "infrastructure/dns/wire/framing.h"
#include "infrastructure/network/transport/tcp_stream.h"
#include "infrastructure/network/transport/udp_socket.h"
#include "support/fmt.hpp"

namespace dns::detail {
namespace {

/// Per-query budget for one UDP exchange, restored from the legacy resolver:
/// a server that silently drops the query fails as RETRY (retryable at the
/// dispatcher level) instead of parking until the caller's scope fires.
constexpr auto UDP_BUDGET = std::chrono::seconds(1);

/// Per-operation budget for one classic TCP fallback (connect, send, read do
/// not share one clock), restored from the legacy resolver.
constexpr auto TCP_OP_BUDGET = std::chrono::seconds(1);

[[nodiscard]] domain::DnsErrorInfo connection_error(const char* stage) {
    return domain::DnsErrorInfo{domain::DnsError::CONNECTION, fmt::format("DNS {} failed", stage)};
}

/// A budget expiry is the legacy transport timeout: RETRY, so the dispatcher
/// retries (single resolver) or fails over (multiple resolvers).
[[nodiscard]] domain::DnsErrorInfo budget_exceeded(const char* stage) {
    return domain::DnsErrorInfo{domain::DnsError::RETRY, fmt::format("DNS {} timed out", stage)};
}

/// One UDP exchange: send the query, then wait for the answer from the asked
/// server only. The socket is never bound explicitly: it opens lazily on the
/// first send and the kernel picks a same-family wildcard with an ephemeral
/// port, which keeps IPv6 servers working (an explicit wildcard bind would
/// have to match the family). Cancellation throws coro::Cancelled;
/// values.
[[nodiscard]] coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> exchange_udp(
    const domain::InetAddress server, const std::uint16_t port, const std::span<const std::uint8_t> query) {
    net::UdpSocket socket{server.get_family()};
    if (auto sent = co_await socket.send_to(server, port, query); !sent) {
        co_return std::unexpected(connection_error("send"));
    }

    // The answer must come from the server that was asked; anything else is a
    // stray datagram and is dropped without extending the caller's budget.
    for (;;) {
        std::array<std::uint8_t, dns::MAX_MESSAGE_SIZE> buffer{};
        auto received = co_await socket.recv_from(buffer);
        if (!received) {
            co_return std::unexpected(connection_error("receive"));
        }
        if (received->from != server || received->port != port) {
            continue;
        }
        co_return std::vector<std::uint8_t>(buffer.begin(),
                                            buffer.begin() + static_cast<std::ptrdiff_t>(received->size));
    }
}

}  // namespace

coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> query_udp(
    const domain::InetAddress server, const std::uint16_t port, const std::span<const std::uint8_t> query) {
    // The send and the wait share one budget, as the legacy deadline did.
    auto outcome = co_await coro::with_timeout(
        UDP_BUDGET,
        [server, port, query]() -> coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> {
            co_return co_await exchange_udp(server, port, query);
        });
    // timed_out implies cancelled (the timer marks the scope), so it must be
    // checked first: an expired own budget is the legacy timeout, anything
    // else is the body's own result, including outer cancellation.
    if (outcome.timed_out) {
        co_return std::unexpected(budget_exceeded("UDP query"));
    }
    co_return std::move(*outcome);
}

coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> query_tcp(
    const domain::InetAddress server, const std::uint16_t port, const std::span<const std::uint8_t> query) {
    net::TcpStream stream{server, port};

    auto connected = co_await coro::with_timeout(
        TCP_OP_BUDGET,
        [&stream]() -> coro::Task<std::expected<void, net::IoError>> { co_return co_await stream.ensure_connected(); });
    if (connected.timed_out) {
        co_return std::unexpected(budget_exceeded("TCP connect"));
    }
    if (!*connected) {
        co_return std::unexpected(connection_error("connect"));
    }

    const auto framed = dns::frame_message(query);
    if (!framed) {
        co_return std::unexpected(
            domain::DnsErrorInfo{domain::DnsError::PARSE, "DNS query exceeds the classic TCP limit"});
    }

    auto sent = co_await coro::with_timeout(TCP_OP_BUDGET,
                                            [&stream, &framed]() -> coro::Task<std::expected<void, net::IoError>> {
                                                co_return co_await stream.send_all(*framed);
                                            });
    if (sent.timed_out) {
        co_return std::unexpected(budget_exceeded("TCP send"));
    }
    if (!*sent) {
        co_return std::unexpected(connection_error("send"));
    }

    std::array<std::uint8_t, 2> prefix{};
    auto got_prefix = co_await coro::with_timeout(
        TCP_OP_BUDGET, [&stream, &prefix]() -> coro::Task<std::expected<void, net::IoError>> {
            co_return co_await stream.read_exact(prefix);
        });
    if (got_prefix.timed_out) {
        co_return std::unexpected(budget_exceeded("TCP receive"));
    }
    if (!*got_prefix) {
        co_return std::unexpected(connection_error("receive"));
    }

    const auto length = dns::read_length(prefix);
    if (!length) {
        co_return std::unexpected(
            domain::DnsErrorInfo{domain::DnsError::PARSE, "DNS server announced an invalid TCP message length"});
    }

    std::vector<std::uint8_t> response(*length);
    auto got_body = co_await coro::with_timeout(
        TCP_OP_BUDGET, [&stream, &response]() -> coro::Task<std::expected<void, net::IoError>> {
            co_return co_await stream.read_exact(response);
        });
    if (got_body.timed_out) {
        co_return std::unexpected(budget_exceeded("TCP receive"));
    }
    if (!*got_body) {
        co_return std::unexpected(connection_error("receive"));
    }
    co_return response;
}

}  // namespace dns::detail
