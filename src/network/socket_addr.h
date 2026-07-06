//
// Created by Kotarou on 2026/7/6.
//

#ifndef YADDNSC_NETWORK_SOCKET_ADDR_H
#define YADDNSC_NETWORK_SOCKET_ADDR_H

#include <cstdint>
#include <optional>
#include <string>

#include <sys/socket.h>

class InetAddress;

// ---------------------------------------------------------------------------
// SocketAddr — type-safe C++ wrapper around sockaddr_storage.
//
// Bridges the POSIX C socket API and the project's InetAddress value types.
// Holds IPv4 or IPv6 address + port in a single value type.
// ---------------------------------------------------------------------------
class SocketAddr {
public:
    /// Default-constructs an empty (AF_UNSPEC) address.
    SocketAddr() noexcept = default;

    // ---- factory methods ---------------------------------------------------

    /// Build from an InetAddress + port.
    /// Returns std::nullopt if the address family is not AF_INET or AF_INET6.
    static std::optional<SocketAddr> from_inet(const InetAddress &addr, std::uint16_t port);

    /// Build from a raw POSIX sockaddr (copies the data internally).
    static SocketAddr from_raw(const sockaddr *addr, socklen_t len);

    // ---- accessors ---------------------------------------------------------

    [[nodiscard]] int family() const noexcept { return storage_.ss_family; }

    /// Port in host byte order.
    [[nodiscard]] std::uint16_t port() const noexcept;

    /// Parse the address portion as an InetAddress.
    [[nodiscard]] std::optional<InetAddress> address() const;

    /// Human-readable "addr:port" string.
    [[nodiscard]] std::string to_string() const;

    // ---- C API interop (for passing to raw syscalls) ----------------------

    [[nodiscard]] const sockaddr *raw() const noexcept {
        return reinterpret_cast<const sockaddr *>(&storage_);
    }

    [[nodiscard]] socklen_t raw_len() const noexcept { return len_; }

    /// Mutable raw pointer for recvfrom / accept to fill in.
    [[nodiscard]] sockaddr *raw_mut() noexcept {
        return reinterpret_cast<sockaddr *>(&storage_);
    }

    [[nodiscard]] socklen_t *raw_len_ptr() noexcept { return &len_; }

private:
    sockaddr_storage storage_{};
    socklen_t len_{sizeof(storage_)};
};

#endif // YADDNSC_NETWORK_SOCKET_ADDR_H
