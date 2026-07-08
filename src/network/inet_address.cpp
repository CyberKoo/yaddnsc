//
// Created by Kotarou on 2026/6/29.
//

#include "inet_address.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <span>
#include <algorithm>
#include <cstdint>
#include <charconv>
#include <array>

// ===========================================================================
// Inet4Address
// ===========================================================================

std::optional<Inet4Address> Inet4Address::parse(std::string_view addr) {
    if (addr.empty()) {
        return std::nullopt;
    }

    const auto s = std::string(addr);

    in_addr in{};
    if (inet_pton(AF_INET, s.c_str(), &in) != 1) {
        return std::nullopt;
    }

    Inet4Address result;
    const auto *src = reinterpret_cast<const std::uint8_t *>(&in.s_addr);
    std::ranges::copy(src, src + ADDR_LEN, result.addr_.begin());
    return result;
}

Inet4Address Inet4Address::from_bytes(const addr_type &bytes) noexcept {
    Inet4Address result;
    result.addr_ = bytes;
    return result;
}

Inet4Address Inet4Address::from_array(const addr_type &addr) noexcept {
    return from_bytes(addr);
}

std::string Inet4Address::to_string() const {
    std::array<char, INET_ADDRSTRLEN> buf{};
    if (inet_ntop(AF_INET, addr_.data(), buf.data(), buf.size()) == nullptr) {
        return "<invalid>";
    }
    return buf.data();
}

// ===========================================================================
// Inet6Address
// ===========================================================================

std::optional<Inet6Address> Inet6Address::parse(std::string_view addr) {
    if (addr.empty()) {
        return std::nullopt;
    }

    auto s = std::string(addr);

    // Extract scope ID if present ("fe80::1%eth0").
    std::uint32_t scope_id = 0;
    if (s.contains('%')) {
        auto pct = s.find('%');
        auto scope_str = s.substr(pct + 1);
        s.resize(pct);

        if (!scope_str.empty()) {
            unsigned long val = 0;
            auto [ptr, ec] = std::from_chars(scope_str.data(), scope_str.data() + scope_str.size(), val);
            if (ec == std::errc{} && ptr == scope_str.data() + scope_str.size()) {
                scope_id = static_cast<std::uint32_t>(val);
            }
        }
    }

    in6_addr in{};
    if (inet_pton(AF_INET6, s.c_str(), &in) != 1) {
        return std::nullopt;
    }

    Inet6Address result;
    std::ranges::copy(in.s6_addr, result.addr_.begin());
    result.scope_id_ = scope_id;
    return result;
}

Inet6Address Inet6Address::from_bytes(const addr_type &bytes) noexcept {
    Inet6Address result;
    result.addr_ = bytes;
    return result;
}

Inet6Address Inet6Address::from_array(const addr_type &addr) noexcept {
    return from_bytes(addr);
}

std::string Inet6Address::to_string() const {
    std::array<char, INET6_ADDRSTRLEN> buf{};
    if (inet_ntop(AF_INET6, addr_.data(), buf.data(), buf.size()) == nullptr) {
        return "<invalid>";
    }

    if (scope_id_ > 0) {
        return std::string(buf.data()) + "%" + std::to_string(scope_id_);
    }

    return buf.data();
}

// ===========================================================================
// InetAddress (variant wrapper)
// ===========================================================================

std::optional<InetAddress> InetAddress::parse(std::string_view addr) {
    if (addr.empty()) {
        return std::nullopt;
    }

    // Try IPv4 first (cheaper).
    if (auto v4 = Inet4Address::parse(addr)) {
        return InetAddress{*v4};
    }

    // Then try IPv6.
    if (auto v6 = Inet6Address::parse(addr)) {
        return InetAddress{*v6};
    }

    return std::nullopt;
}

std::optional<InetAddress> InetAddress::from_bytes(std::span<const std::uint8_t> bytes) {
    switch (bytes.size()) {
        case 4: {
            Inet4Address::addr_type arr{};
            std::ranges::copy(bytes, arr.begin());
            return InetAddress{Inet4Address::from_bytes(arr)};
        }
        case 16: {
            Inet6Address::addr_type arr{};
            std::ranges::copy(bytes, arr.begin());
            return InetAddress{Inet6Address::from_bytes(arr)};
        }
        default:
            return std::nullopt;
    }
}

AddressFamily InetAddress::get_family() const noexcept {
    return std::visit([](const auto &a) { return a.get_family(); }, addr_);
}

std::string InetAddress::to_string() const {
    return std::visit([](const auto &a) { return a.to_string(); }, addr_);
}

std::array<std::uint8_t, 16> InetAddress::get_address() const {
    std::array<std::uint8_t, 16> result{};
    std::visit([&result](const auto &a) { std::ranges::copy(a.addr(), result.begin()); }, addr_);
    return result;
}

bool InetAddress::is_loopback() const {
    return std::visit([](const auto &a) { return a.is_loopback(); }, addr_);
}

bool InetAddress::is_multicast() const {
    return std::visit([](const auto &a) { return a.is_multicast(); }, addr_);
}

bool InetAddress::is_unspecified() const {
    return std::visit([](const auto &a) { return a.is_unspecified(); }, addr_);
}

bool InetAddress::is_link_local() const noexcept {
    auto *v6 = as_v6();
    return v6 && v6->is_link_local();
}

bool InetAddress::is_site_local() const noexcept {
    auto *v6 = as_v6();
    return v6 && v6->is_site_local();
}

bool InetAddress::is_ula() const noexcept {
    auto *v6 = as_v6();
    return v6 && v6->is_ula();
}

std::uint32_t InetAddress::get_scope_id() const noexcept {
    auto *v6 = as_v6();
    return v6 ? v6->get_scope_id() : 0;
}
