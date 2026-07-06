//
// Created by Kotarou on 2026/7/6.
//
#include "network/socket_addr.h"

#include <arpa/inet.h>

#include <cstring>
#include <utility>
#include <cstdint>

#include "network/inet_address.h"

// ===========================================================================
//  Factory methods
// ===========================================================================

std::optional<SocketAddr> SocketAddr::from_inet(const InetAddress &addr, std::uint16_t port) {
    SocketAddr result;

    addr.visit([&](const auto &concrete) {
        using T = std::decay_t<decltype(concrete)>;

        if constexpr (std::is_same_v<T, Inet4Address>) {
            auto &sin = *reinterpret_cast<sockaddr_in *>(&result.storage_);
            sin.sin_family = AF_INET;
            sin.sin_port = htons(port);
            std::memcpy(&sin.sin_addr, concrete.data(), 4);
            result.len_ = sizeof(sin);

        } else if constexpr (std::is_same_v<T, Inet6Address>) {
            auto &sin6 = *reinterpret_cast<sockaddr_in6 *>(&result.storage_);
            sin6.sin6_family = AF_INET6;
            sin6.sin6_port = htons(port);
            sin6.sin6_flowinfo = 0;
            std::memcpy(&sin6.sin6_addr, concrete.data(), 16);
            sin6.sin6_scope_id = concrete.get_scope_id();
            result.len_ = sizeof(sin6);
        }
    });

    if (result.storage_.ss_family == AF_UNSPEC) {
        return std::nullopt;
    }
    return result;
}

SocketAddr SocketAddr::from_raw(const sockaddr *addr, socklen_t len) {
    SocketAddr result;
    if (addr && len > 0 && static_cast<size_t>(len) <= sizeof(result.storage_)) {
        std::memcpy(&result.storage_, addr, static_cast<size_t>(len));
        result.len_ = len;
    }
    return result;
}

// ===========================================================================
//  Accessors
// ===========================================================================

std::uint16_t SocketAddr::port() const noexcept {
    switch (storage_.ss_family) {
        case AF_INET:
            return ntohs(reinterpret_cast<const sockaddr_in *>(&storage_)->sin_port);
        case AF_INET6:
            return ntohs(reinterpret_cast<const sockaddr_in6 *>(&storage_)->sin6_port);
        default:
            return 0;
    }
}

std::optional<InetAddress> SocketAddr::address() const {
    switch (storage_.ss_family) {
        case AF_INET: {
            const auto &sin = reinterpret_cast<const sockaddr_in *>(&storage_);
            Inet4Address::addr_type bytes{};
            std::memcpy(bytes.data(), &sin->sin_addr, 4);
            return InetAddress{Inet4Address::from_bytes(bytes)};
        }
        case AF_INET6: {
            const auto &sin6 = reinterpret_cast<const sockaddr_in6 *>(&storage_);
            Inet6Address::addr_type bytes{};
            std::memcpy(bytes.data(), &sin6->sin6_addr, 16);
            auto v6 = Inet6Address::from_bytes(bytes);
            if (sin6->sin6_scope_id != 0) {
                v6.set_scope_id(sin6->sin6_scope_id);
            }
            return InetAddress{std::move(v6)};
        }
        default:
            return std::nullopt;
    }
}

std::string SocketAddr::to_string() const {
    auto inet = address();
    if (!inet) {
        return "<unspec>";
    }

    auto addr_str = inet->to_string();
    auto p = port();

    if (storage_.ss_family == AF_INET6) {
        return "[" + addr_str + "]:" + std::to_string(p);
    }
    return addr_str + ":" + std::to_string(p);
}
