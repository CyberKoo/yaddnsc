//
// One classic UDP DNS exchange.
//
#include "classic_udp.h"

#include <cerrno>
#include <utility>
#include <vector>

#include <poll.h>
#include <spdlog/spdlog.h>
#include <sys/socket.h>

#include "domain/error/dns_error.h"
#include "domain/network/inet_address.h"
#include "infrastructure/dns/classic/socket_error.hpp"
#include "infrastructure/dns/wire/framing.h"
#include "infrastructure/network/socket.h"
#include "infrastructure/network/socket_addr.h"
#include "support/fmt.hpp"
#include "support/util/cancellation_token.hpp"

namespace DNS {
namespace {

[[nodiscard]] bool same_source(const SocketAddr& got, const SocketAddr& expected) {
    return got.family() == expected.family() && got.port() == expected.port() && got.address().has_value() &&
           expected.address().has_value() && *got.address() == *expected.address();
}

}  // namespace

std::expected<std::vector<std::uint8_t>, DnsErrorInfo> exchange_udp(
    const SocketAddr& server, const std::span<const std::uint8_t> query,
    const std::chrono::steady_clock::time_point deadline, const Utils::CancellationToken& token,
    const std::uint64_t resolver_id) {
    if (token.is_triggered()) {
        return std::unexpected(
            DnsErrorInfo{DnsError::CANCELLED, fmt::format(R"(Resolver #{} UDP query cancelled)", resolver_id)});
    }
    if (std::chrono::steady_clock::now() >= deadline) {
        return std::unexpected(
            DnsErrorInfo{DnsError::RETRY, fmt::format(R"(Resolver #{} UDP query timed out)", resolver_id)});
    }

    auto opened = Socket::open(server.family(), SOCK_DGRAM);
    if (!opened) {
        return std::unexpected(exchange_error(resolver_id, "UDP", "socket", opened.error()));
    }
    Socket sock = std::move(*opened);
    if (auto nb = sock.set_nonblocking(true); !nb) {
        return std::unexpected(exchange_error(resolver_id, "UDP", "nonblocking", nb.error()));
    }

    const auto data = std::as_bytes(query);
    for (;;) {
        if (token.is_triggered()) {
            return std::unexpected(
                DnsErrorInfo{DnsError::CANCELLED, fmt::format(R"(Resolver #{} UDP query cancelled)", resolver_id)});
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::unexpected(
                DnsErrorInfo{DnsError::RETRY, fmt::format(R"(Resolver #{} UDP query timed out)", resolver_id)});
        }

        auto sent = sock.send_to(data, server);
        if (sent && *sent == data.size()) {
            break;
        }
        if (!sent && retryable_io(sent.error())) {
            auto ready = sock.wait_until(POLLOUT, deadline, token);
            if (!ready) {
                return std::unexpected(exchange_error(resolver_id, "UDP", "send", ready.error()));
            }
            continue;
        }
        const int err = sent ? EMSGSIZE : sent.error();
        return std::unexpected(exchange_error(resolver_id, "UDP", "sendto", err));
    }

    std::vector<std::uint8_t> response(MAX_MESSAGE_SIZE);
    const auto buf = std::as_writable_bytes(std::span{response});

    // Discard datagrams from unexpected sources under the same deadline.
    // A spoofed response must not abort the query or refresh the budget.
    for (;;) {
        if (token.is_triggered()) {
            return std::unexpected(
                DnsErrorInfo{DnsError::CANCELLED, fmt::format(R"(Resolver #{} UDP query cancelled)", resolver_id)});
        }

        auto ready = sock.wait_until(POLLIN, deadline, token);
        if (!ready) {
            if (ready.error() == ETIMEDOUT) {
                return std::unexpected(
                    DnsErrorInfo{DnsError::RETRY, fmt::format(R"(Resolver #{} UDP query timed out)", resolver_id)});
            }
            return std::unexpected(exchange_error(resolver_id, "UDP", "wait", ready.error()));
        }

        SocketAddr src;
        auto received = sock.recv_from(buf, &src);
        if (!received) {
            if (retryable_io(received.error())) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    return std::unexpected(
                        DnsErrorInfo{DnsError::RETRY, fmt::format(R"(Resolver #{} UDP query timed out)", resolver_id)});
                }
                continue;
            }
            return std::unexpected(exchange_error(resolver_id, "UDP", "recvfrom", received.error()));
        }

        if (!same_source(src, server)) {
            SPDLOG_TRACE(R"(Resolver #{} discarding UDP response from unexpected source "{}")", resolver_id,
                         src.to_string());
            continue;
        }

        response.resize(*received);
        return response;
    }
}

}  // namespace DNS
