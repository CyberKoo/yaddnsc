//
// Classic DNS over TCP to an already-resolved address.
//
#include "classic_tcp.h"

#include <array>
#include <cstring>
#include <utility>
#include <vector>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <spdlog/spdlog.h>

#include "domain/error/dns_error.h"
#include "infrastructure/dns/classic/socket_error.hpp"
#include "infrastructure/dns/wire/framing.h"
#include "infrastructure/network/socket.h"
#include "infrastructure/network/socket_addr.h"
#include "infrastructure/network/tcp_transfer.h"
#include "support/fmt.hpp"
#include "support/util/cancellation_token.hpp"

namespace DNS {

std::expected<std::vector<std::uint8_t>, DnsErrorInfo> exchange_tcp(
    const SocketAddr& server, const std::span<const std::uint8_t> query,
    const std::chrono::steady_clock::time_point deadline, const Utils::CancellationToken& token,
    const std::uint64_t resolver_id) {
    if (token.is_triggered()) {
        return std::unexpected(
            DnsErrorInfo{DnsError::CANCELLED, fmt::format(R"(Resolver #{} TCP query cancelled)", resolver_id)});
    }
    if (std::chrono::steady_clock::now() >= deadline) {
        return std::unexpected(
            DnsErrorInfo{DnsError::RETRY, fmt::format(R"(Resolver #{} TCP query timed out)", resolver_id)});
    }

    auto framed = frame_message(query);
    if (!framed) {
        return std::unexpected(
            DnsErrorInfo{DnsError::PARSE, fmt::format(R"(Resolver #{} TCP query exceeds 65535 bytes)", resolver_id)});
    }

    auto opened = Socket::open(server.family(), SOCK_STREAM, IPPROTO_TCP);
    if (!opened) {
        return std::unexpected(exchange_error(resolver_id, "TCP", "socket", opened.error()));
    }
    Socket sock = std::move(*opened);

    // One request/response. Nagle would hold the small write; a failure
    // leaves the socket usable.
    const int nodelay = 1;
    if (auto option = sock.set_option(IPPROTO_TCP, TCP_NODELAY, nodelay); !option) {
        SPDLOG_DEBUG(R"(Resolver #{} TCP_NODELAY failed: {})", resolver_id, std::strerror(option.error()));
    }

    if (auto connected = sock.connect(server, deadline, token); !connected) {
        return std::unexpected(exchange_error(resolver_id, "TCP", "connect", connected.error()));
    }

    if (auto sent = tcp_send_all(sock, std::as_bytes(std::span{*framed}), deadline, token); !sent) {
        return std::unexpected(exchange_error(resolver_id, "TCP", "send", sent.error()));
    }

    std::array<std::uint8_t, 2> len_buf{};
    if (auto got = tcp_read_exact(sock, std::as_writable_bytes(std::span{len_buf}), deadline, token); !got) {
        return std::unexpected(exchange_error(resolver_id, "TCP", "recv", got.error()));
    }

    const auto rsp_len = read_length(std::span{len_buf});
    if (!rsp_len) {
        return std::unexpected(DnsErrorInfo{
            DnsError::PARSE,
            fmt::format("Invalid DNS response length: {}", announced_length(std::span{len_buf}))});
    }

    std::vector<std::uint8_t> response(*rsp_len);
    if (auto got = tcp_read_exact(sock, std::as_writable_bytes(std::span{response}), deadline, token); !got) {
        return std::unexpected(exchange_error(resolver_id, "TCP", "recv", got.error()));
    }
    return response;
}

}  // namespace DNS
