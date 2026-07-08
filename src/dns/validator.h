//
// Created by Kotarou on 2026/7/7.
//
#ifndef YADDNSC_DNS_VALIDATOR_H
#define YADDNSC_DNS_VALIDATOR_H

#include <cstdint>
#include <span>

/// Validate a DNS response against its request.
///
/// Checks (in order):
///   1. Minimum header size (RFC 1035 §4.1.1)
///   2. QR bit set (this is a response)
///   3. Transaction ID matches the request
///   4. QDCOUNT == 1
///   5. Question section is echoed verbatim (RFC 1035 §4.1.2)
///
/// @throws DnsLookupException with Error::PARSE on any failure.
namespace DNS {
    namespace Validator {
        void validate_response(std::span<const std::uint8_t> request,
                               std::span<const std::uint8_t> response);
    } // namespace Validator
} // namespace DNS

#endif // YADDNSC_DNS_VALIDATOR_H
