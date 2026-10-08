//
// dns — shared classic wire exchanges over the coroutine transport.
//

#include "exchange.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "domain/dns/record_kind.h"
#include "infrastructure/dns/wire/framing.h"
#include "infrastructure/net/tcp_stream.h"
#include "infrastructure/net/udp_socket.h"
#include "support/fmt.hpp"

namespace dns::detail {
namespace {

[[nodiscard]] DnsErrorInfo map_io_error(const net::IoError error, const char* stage) {
    if (error == net::IoError::CANCELLED) {
        return DnsErrorInfo{DnsError::CANCELLED, fmt::format("DNS {} cancelled", stage)};
    }
    return DnsErrorInfo{DnsError::CONNECTION, fmt::format("DNS {} failed", stage)};
}

}  // namespace

coro::Task<std::expected<std::vector<std::uint8_t>, DnsErrorInfo>> query_udp(
    const InetAddress server, const std::uint16_t port, const std::span<const std::uint8_t> query) {
    net::UdpSocket socket{server.get_family()};
    if (auto bound = socket.bind(InetAddress{}, 0); !bound) {
        co_return std::unexpected(DnsErrorInfo{DnsError::CONNECTION, "DNS UDP socket could not be bound"});
    }
    if (auto sent = co_await socket.send_to(server, port, query); !sent) {
        co_return std::unexpected(map_io_error(sent.error(), "send"));
    }

    // The answer must come from the server that was asked; anything else is a
    // stray datagram and is dropped without extending the caller's budget.
    for (;;) {
        std::array<std::uint8_t, dns::MAX_MESSAGE_SIZE> buffer{};
        auto received = co_await socket.recv_from(buffer);
        if (!received) {
            co_return std::unexpected(map_io_error(received.error(), "receive"));
        }
        if (received->from != server || received->port != port) {
            continue;
        }
        co_return std::vector<std::uint8_t>(buffer.begin(),
                                            buffer.begin() + static_cast<std::ptrdiff_t>(received->size));
    }
}

coro::Task<std::expected<std::vector<std::uint8_t>, DnsErrorInfo>> query_tcp(
    const InetAddress server, const std::uint16_t port, const std::span<const std::uint8_t> query) {
    net::TcpStream stream{server, port};
    if (auto connected = co_await stream.ensure_connected(); !connected) {
        co_return std::unexpected(map_io_error(connected.error(), "connect"));
    }

    const auto framed = dns::frame_message(query);
    if (!framed) {
        co_return std::unexpected(DnsErrorInfo{DnsError::PARSE, "DNS query exceeds the classic TCP limit"});
    }
    if (auto sent = co_await stream.send_all(*framed); !sent) {
        co_return std::unexpected(map_io_error(sent.error(), "send"));
    }

    std::array<std::uint8_t, 2> prefix{};
    if (auto got = co_await stream.read_exact(prefix); !got) {
        co_return std::unexpected(map_io_error(got.error(), "receive"));
    }
    const auto length = dns::read_length(prefix);
    if (!length) {
        co_return std::unexpected(DnsErrorInfo{DnsError::PARSE, "DNS server announced an invalid TCP message length"});
    }

    std::vector<std::uint8_t> response(*length);
    if (auto got = co_await stream.read_exact(response); !got) {
        co_return std::unexpected(map_io_error(got.error(), "receive"));
    }
    co_return response;
}

}  // namespace dns::detail
