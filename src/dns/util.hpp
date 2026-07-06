//
// Created by Kotarou on 2026/6/28.
//

#ifndef YADDNSC_DNS_UTIL_H
#define YADDNSC_DNS_UTIL_H

#include <arpa/nameser.h>

#include <cstddef>
#include <cstdint>
#include <span>

#include "dns_type.h"
#include "address_family.h"

/// DNS utility — compile-time type conversion helpers.
namespace DNS::Util {

    /// Read a 16-bit big-endian value from a raw pointer.
    [[nodiscard]] constexpr std::uint16_t read_u16_be(const std::uint8_t *buf) noexcept {
        return (static_cast<std::uint16_t>(buf[0]) << 8) | buf[1];
    }

    /// Read a 16-bit big-endian value from a span at the given offset.
    [[nodiscard]] inline std::uint16_t read_u16_be(std::span<const std::uint8_t> buf, std::size_t offset) noexcept {
        return read_u16_be(buf.data() + offset);
    }

    /// Convert DNS record type to address family (A → IPV4, AAAA → IPV6).
    /// @param type  The DNS record type.
    /// @return      The corresponding AddressFamily, or UNSPECIFIED for non-address types.
    constexpr AddressFamily type_to_family(Type type) noexcept {
        switch (type) {
            case Type::A:    return AddressFamily::IPV4;
            case Type::AAAA: return AddressFamily::IPV6;
            default:         return AddressFamily::UNSPECIFIED;
        }
    }

    /// Convert DNS record type to the corresponding `<arpa/nameser.h>` ns_t_* constant.
    /// @param type  The DNS record type.
    /// @return      The ns_t_* constant for use with libresolv, or ns_t_invalid.
    constexpr int to_ns_type(Type type) noexcept {
        switch (type) {
            case Type::A:    return ns_t_a;
            case Type::AAAA: return ns_t_aaaa;
            case Type::TXT:  return ns_t_txt;
            case Type::SOA:  return ns_t_soa;
            default:         return ns_t_invalid;
        }
    }

} // namespace DNS

#endif // YADDNSC_DNS_UTIL_H
