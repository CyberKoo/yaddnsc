//
// Created by Kotarou on 2026/7/7.
//
#ifndef YADDNSC_DNS_VALIDATOR_H
#define YADDNSC_DNS_VALIDATOR_H

#include <cstdint>
#include <expected>
#include <span>

#include "dns/dns_error_info.h"

/// Validate a DNS response against its request.
///
/// Checks (in order):
///   1. Minimum header size (RFC 1035 §4.1.1)
///   2. QR bit set (this is a response)
///   3. Transaction ID matches the request
///   4. QDCOUNT == 1
///   5. Question section is echoed verbatim (RFC 1035 §4.1.2)
///
/// @return  std::expected<void, DnsErrorInfo> — success or a PARSE error.
namespace DNS::Validator {
    [[nodiscard]] std::expected<void, DnsErrorInfo> validate_response(
        std::span<const std::uint8_t> request, std::span<const std::uint8_t> response);
} // namespace DNS

#endif // YADDNSC_DNS_VALIDATOR_H
