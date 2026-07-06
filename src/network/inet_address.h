//
// Created by Kotarou on 2026/6/29.
//

#ifndef YADDNSC_NETWORK_INET_ADDRESS_H
#define YADDNSC_NETWORK_INET_ADDRESS_H

#include <array>
#include <span>
#include <string>
#include <cstdint>
#include <variant>
#include <optional>
#include <string_view>

#include "address_family.h"

// ---------------------------------------------------------------------------
// Inet4Address — lightweight IPv4 address value type.
// ---------------------------------------------------------------------------
class Inet4Address {
public:
    static constexpr size_t ADDR_LEN = 4;
    using addr_type = std::array<std::uint8_t, ADDR_LEN>;

    /// Default-constructs 0.0.0.0 (unspecified).
    Inet4Address() noexcept = default;

    // ---- factory methods ---------------------------------------------------

    /// Parse a dotted-decimal IPv4 string.  Returns std::nullopt on failure.
    static std::optional<Inet4Address> parse(std::string_view addr);

    /// Create from 4 raw bytes (network byte order).
    static Inet4Address from_bytes(const addr_type &bytes);

    static Inet4Address from_array(const addr_type &addr);

    // ---- accessors ---------------------------------------------------------

    static constexpr AddressFamily get_family() noexcept { return AddressFamily::IPV4; }

    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] constexpr const addr_type &get_address() const noexcept { return addr_; }

    // ---- classification ----------------------------------------------------

    [[nodiscard]] constexpr bool is_loopback() const noexcept { return addr_[0] == 127; }

    [[nodiscard]] constexpr bool is_multicast() const noexcept { return (addr_[0] & 0xf0) == 0xe0; }

    [[nodiscard]] constexpr bool is_unspecified() const noexcept {
        static constexpr addr_type zero{};
        return addr_ == zero;
    }

    // ---- equality ----------------------------------------------------------

    bool operator==(const Inet4Address &other) const = default;

    // ---- direct access -----------------------------------------------------

    [[nodiscard]] constexpr const std::uint8_t *data() const noexcept { return addr_.data(); }

    [[nodiscard]] constexpr const addr_type &addr() const noexcept { return addr_; }

private:
    addr_type addr_{};
};

// ---------------------------------------------------------------------------
// Inet6Address — lightweight IPv6 address value type.
// ---------------------------------------------------------------------------
class Inet6Address {
public:
    static constexpr size_t ADDR_LEN = 16;
    using addr_type = std::array<std::uint8_t, ADDR_LEN>;

    /// Default-constructs :: (unspecified).
    Inet6Address() noexcept = default;

    // ---- factory methods ---------------------------------------------------

    /// Parse an IPv6 string (with or without scope ID, e.g. "fe80::1%eth0").
    /// Returns std::nullopt on failure.
    static std::optional<Inet6Address> parse(std::string_view addr);

    /// Create from 16 raw bytes (network byte order).
    static Inet6Address from_bytes(const addr_type &bytes);

    static Inet6Address from_array(const addr_type &addr);

    // ---- accessors ---------------------------------------------------------

    static constexpr AddressFamily get_family() noexcept { return AddressFamily::IPV6; }

    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] constexpr const addr_type &get_address() const noexcept { return addr_; }

    // ---- classification ----------------------------------------------------

    [[nodiscard]] constexpr bool is_loopback() const noexcept {
        static constexpr addr_type loopback{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        return addr_ == loopback;
    }

    [[nodiscard]] constexpr bool is_multicast() const noexcept { return addr_[0] == 0xff; }

    [[nodiscard]] constexpr bool is_unspecified() const noexcept {
        static constexpr addr_type zero{};
        return addr_ == zero;
    }

    [[nodiscard]] constexpr bool is_link_local() const noexcept {
        return addr_[0] == 0xfe && (addr_[1] & 0xc0) == 0x80;
    }

    [[nodiscard]] constexpr bool is_site_local() const noexcept {
        return addr_[0] == 0xfe && (addr_[1] & 0xc0) == 0xc0;
    }

    [[nodiscard]] constexpr bool is_ula() const noexcept { return (addr_[0] & 0xfe) == 0xfc; }

    // ---- scope ID ----------------------------------------------------------

    [[nodiscard]] std::uint32_t get_scope_id() const noexcept { return scope_id_; }
    void set_scope_id(std::uint32_t id) noexcept { scope_id_ = id; }

    // ---- equality ----------------------------------------------------------

    bool operator==(const Inet6Address &other) const = default;

    // ---- direct access -----------------------------------------------------

    [[nodiscard]] constexpr const std::uint8_t *data() const noexcept { return addr_.data(); }

    [[nodiscard]] constexpr const addr_type &addr() const noexcept { return addr_; }

private:
    addr_type addr_{};
    std::uint32_t scope_id_{0};
};

// ---------------------------------------------------------------------------
// InetAddress — type-erased wrapper over Inet4Address / Inet6Address.
//
// Replaces the old virtual-inheritance hierarchy with a std::variant-based
// value type, eliminating heap allocation and virtual dispatch overhead.
// ---------------------------------------------------------------------------
class InetAddress {
public:
    using variant_type = std::variant<Inet4Address, Inet6Address>;

    /// Default-constructs holding Inet4Address{} (0.0.0.0).
    InetAddress() noexcept = default;

    // implicit conversions from concrete types
    // NOLINTNEXTLINE(google-explicit-constructor)
    InetAddress(Inet4Address v4) noexcept : addr_(v4) {
    }

    // NOLINTNEXTLINE(google-explicit-constructor)
    InetAddress(Inet6Address v6) noexcept : addr_(v6) {
    }

    // ---- factory methods ---------------------------------------------------

    /// Parse a textual IP address ("192.168.1.1" or "::1").
    /// Returns std::nullopt on parse failure.
    static std::optional<InetAddress> parse(std::string_view addr);

    /// Parse from an opaque byte buffer.  `len` must be 4 (IPv4) or 16 (IPv6);
    /// returns std::nullopt otherwise.
    [[nodiscard]] static std::optional<InetAddress> from_bytes(std::span<const std::uint8_t> bytes);

    // ---- accessors ---------------------------------------------------------

    [[nodiscard]] AddressFamily get_family() const noexcept;

    [[nodiscard]] std::string to_string() const;

    /// Returns 16 bytes; IPv4 addresses are zero-padded to fill the array.
    [[nodiscard]] std::array<std::uint8_t, 16> get_address() const;

    // ---- classification ----------------------------------------------------

    [[nodiscard]] bool is_loopback() const;

    [[nodiscard]] bool is_multicast() const;

    [[nodiscard]] bool is_unspecified() const;

    // ---- IPv6-specific (returns false for IPv4) ----------------------------

    [[nodiscard]] bool is_link_local() const noexcept;

    [[nodiscard]] bool is_site_local() const noexcept;

    [[nodiscard]] bool is_ula() const noexcept;

    [[nodiscard]] std::uint32_t get_scope_id() const noexcept;

    // ---- type-safe access --------------------------------------------------

    [[nodiscard]] const Inet4Address *as_v4() const noexcept { return std::get_if<Inet4Address>(&addr_); }

    [[nodiscard]] const Inet6Address *as_v6() const noexcept { return std::get_if<Inet6Address>(&addr_); }

    // ---- equality ----------------------------------------------------------

    bool operator==(const InetAddress &other) const = default;

    // ---- visit -------------------------------------------------------------

    template<typename Visitor>
    decltype(auto) visit(Visitor &&vis) const {
        return std::visit(std::forward<Visitor>(vis), addr_);
    }

    template<typename Visitor>
    decltype(auto) visit(Visitor &&vis) {
        return std::visit(std::forward<Visitor>(vis), addr_);
    }

private:
    variant_type addr_;
};

#endif // YADDNSC_NETWORK_INET_ADDRESS_H
