//
// Internal: TcpConnection — cancellable TCP connection establishment.
//
#include "infrastructure/network/transport/detail/tcp_connection.h"

#include <cerrno>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <yaddnsc/util/format.hpp>

#include "domain/error/dns_error.h"
#include "domain/network/inet_address.h"
#include "infrastructure/dns/bootstrap.h"
#include "infrastructure/network/socket_addr.h"
#include "support/fmt.hpp"
#include "support/util/cancellation_token.hpp"
#include "support/util/validation.hpp"

namespace Transport::detail {

TcpConnection::TcpConnection(std::string host, const std::uint16_t port, Options opts)
    : host_(std::move(host)), port_(port), opts_(std::move(opts)) {
    if (!InetAddress::parse(host_).has_value() && !Utils::is_valid_domain(host_)) {
        throw std::invalid_argument(
            fmt::format(R"(Invalid server address: "{}" (not a valid IP or domain name))", host_));
    }
}

std::expected<void, IoError> TcpConnection::connect(const std::chrono::steady_clock::time_point deadline,
                                                    const Utils::CancellationToken& token) {
    using enum IoError;

    close();

    if (token.is_triggered()) {
        return std::unexpected(CANCELLED);
    }
    if (std::chrono::steady_clock::now() >= deadline) {
        return std::unexpected(TIMEOUT);
    }

    std::vector<SocketAddr> addrs;
    if (const auto ip = InetAddress::parse(host_)) {
        if (auto sa = SocketAddr::from_inet(*ip, port_)) {
            addrs.push_back(*sa);
        } else {
            return std::unexpected(CONNECTION_FAILED);
        }
    } else {
        if (opts_.bootstrap_dns.empty()) {
            SPDLOG_WARN(R"(Cannot resolve hostname "{}": no bootstrap DNS servers available. )"
                        R"(Set "bootstrap_dns" in the configuration or populate /etc/resolv.conf, )"
                        R"(or use an IP literal. (/etc/hosts and NSS are not consulted.))",
                        host_);
            return std::unexpected(CONNECTION_FAILED);
        }

        auto resolved = DNS::resolve_bootstrap(host_, opts_.address_family, opts_.bootstrap_dns, deadline, token);
        if (!resolved) {
            if (resolved.error().code == DnsError::CANCELLED) {
                return std::unexpected(CANCELLED);
            }
            if (resolved.error().code == DnsError::RETRY) {
                return std::unexpected(TIMEOUT);
            }
            SPDLOG_DEBUG(R"(Bootstrap name resolution failed for "{}": {})", host_, resolved.error().message);
            return std::unexpected(CONNECTION_FAILED);
        }

        for (const auto& addr : *resolved) {
            if (auto sa = SocketAddr::from_inet(addr, port_)) {
                addrs.push_back(*sa);
            }
        }
        if (addrs.empty()) {
            return std::unexpected(CONNECTION_FAILED);
        }
    }

    for (const auto& sa : addrs) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::unexpected(TIMEOUT);
        }

        auto result = connect_one(sa, deadline, token);
        if (result) {
            return {};
        }
        if (result.error() == TIMEOUT || result.error() == CANCELLED) {
            return std::unexpected(result.error());
        }
    }

    SPDLOG_DEBUG(R"(All connection attempts to "{}:{}" failed)", host_, port_);
    return std::unexpected(CONNECTION_FAILED);
}

std::expected<void, IoError> TcpConnection::connect_one(const SocketAddr& addr,
                                                        const std::chrono::steady_clock::time_point deadline,
                                                        const Utils::CancellationToken& token) {
    using enum IoError;

    auto opened = Socket::open(addr.family(), SOCK_STREAM, IPPROTO_TCP);
    if (!opened) {
        return std::unexpected(CONNECTION_FAILED);
    }
    Socket sock = std::move(*opened);

#ifdef SO_BINDTODEVICE
    if (opts_.interface.has_value() && !opts_.interface->empty()) {
        if (auto bound = sock.set_option_raw(SOL_SOCKET, SO_BINDTODEVICE, opts_.interface->c_str(),
                                             static_cast<socklen_t>(opts_.interface->size()));
            !bound) {
            SPDLOG_WARN(R"(Failed to bind socket to interface "{}": {})", *opts_.interface,
                        std::strerror(bound.error()));
            return std::unexpected(CONNECTION_FAILED);
        }
    }
#else
    if (opts_.interface.has_value() && !opts_.interface->empty()) {
        // Reconnects are periodic, so logging this on every attempt floods the
        // log on platforms where interface binding is unavailable. The
        // capability is process-wide; report it once while preserving the
        // documented best-effort behavior.
        static std::once_flag warned;
        std::call_once(warned, [this] {
            SPDLOG_WARN(R"(Interface binding is not supported on this platform ("{}"), ignoring)", *opts_.interface);
        });
    }
#endif

    auto connected = sock.connect(addr, deadline, token);
    if (!connected) {
        if (connected.error() == ETIMEDOUT) {
            return std::unexpected(TIMEOUT);
        }
        if (connected.error() == ECANCELED) {
            return std::unexpected(CANCELLED);
        }
        return std::unexpected(CONNECTION_FAILED);
    }

    // TCP_NODELAY: disable Nagle — handshakes and request/response exchanges
    // are latency-sensitive. Failure leaves the connection usable.
    const int nodelay = 1;
    if (auto option = sock.set_option(IPPROTO_TCP, TCP_NODELAY, nodelay); !option) {
        SPDLOG_WARN(R"(Failed to set TCP_NODELAY on socket to "{}:{}": {})", host_, port_,
                    std::strerror(option.error()));
    }

    socket_ = std::move(sock);
    return {};
}

void TcpConnection::close() noexcept {
    socket_.close();
}

bool TcpConnection::is_healthy() const noexcept {
    if (socket_.is_closed()) {
        return false;
    }

    // Deadline of "now" is one non-blocking poll: nothing queued means the
    // peer has not closed, and queued bytes or a hang-up are distinguished
    // by a peek.
    const auto ready = socket_.wait_until(POLLIN, std::chrono::steady_clock::now(), {});
    if (!ready) {
        return ready.error() == ETIMEDOUT;
    }

    std::byte peeked{};
    const auto n = socket_.recv_some(std::span{&peeked, 1}, MSG_PEEK);
    if (!n) {
        return n.error() == EAGAIN || n.error() == EWOULDBLOCK;
    }
    return *n > 0;
}

}  // namespace Transport::detail
