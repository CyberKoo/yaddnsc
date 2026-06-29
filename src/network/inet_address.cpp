//
// Created by Kotarou on 2026/6/29.
//

#include "inet_address.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <span>
#include <cstdlib>
#include <algorithm>

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
    const auto *src = reinterpret_cast<const uint8_t *>(&in.s_addr);
    std::ranges::copy(src, src + ADDR_LEN, result.addr_.begin());
    return result;
}

Inet4Address Inet4Address::from_bytes(const addr_type &bytes) {
    Inet4Address result;
    result.addr_ = bytes;
    return result;
}

Inet4Address Inet4Address::from_array(const addr_type &addr) {
    return from_bytes(addr);
}

std::string Inet4Address::to_string() const {
    char buf[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, addr_.data(), buf, sizeof(buf)) == nullptr) {
        return "<invalid>";
    }
    return buf;
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
    uint32_t scope_id = 0;
    if (s.contains('%')) {
        auto pct = s.find('%');
        auto scope_str = s.substr(pct + 1);
        s.resize(pct);

        if (!scope_str.empty()) {
            char *end = nullptr;
            auto val = strtoul(scope_str.c_str(), &end, 10);
            if (end != scope_str.c_str() && *end == '\0') {
                scope_id = static_cast<uint32_t>(val);
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

Inet6Address Inet6Address::from_bytes(const addr_type &bytes) {
    Inet6Address result;
    result.addr_ = bytes;
    return result;
}

Inet6Address Inet6Address::from_array(const addr_type &addr) {
    return from_bytes(addr);
}

std::string Inet6Address::to_string() const {
    char buf[INET6_ADDRSTRLEN];
    if (inet_ntop(AF_INET6, addr_.data(), buf, sizeof(buf)) == nullptr) {
        return "<invalid>";
    }

    if (scope_id_ > 0) {
        return std::string(buf) + "%" + std::to_string(scope_id_);
    }

    return buf;
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
        return InetAddress{std::move(*v4)};
    }

    // Then try IPv6.
    if (auto v6 = Inet6Address::parse(addr)) {
        return InetAddress{std::move(*v6)};
    }

    return std::nullopt;
}

std::optional<InetAddress> InetAddress::from_bytes(std::span<const uint8_t> bytes) {
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

address_family_type InetAddress::get_family() const noexcept {
    return std::visit([](const auto &a) { return a.get_family(); }, addr_);
}

std::string InetAddress::to_string() const {
    return std::visit([](const auto &a) { return a.to_string(); }, addr_);
}

std::array<uint8_t, 16> InetAddress::get_address() const {
    std::array<uint8_t, 16> result{};
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

uint32_t InetAddress::get_scope_id() const noexcept {
    auto *v6 = as_v6();
    return v6 ? v6->get_scope_id() : 0;
}
