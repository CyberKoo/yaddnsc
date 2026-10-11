#ifndef YADDNSC_INFRASTRUCTURE_DNS_VALIDATOR_H
#define YADDNSC_INFRASTRUCTURE_DNS_VALIDATOR_H

#include <cstdint>
#include <span>

#include <expected>

#include "domain/error/dns_error_info.h"

/// Validate a DNS response against its request.
///
/// Checks (in order):
///   1. Minimum header size (RFC 1035 §4.1.1)
///   2. QR bit set (this is a response)
///   3. Transaction ID matches the request
///   4. Question count: exactly one, except for an error answer that legally
///      omits the question section (QDCOUNT == 0 with RCODE != 0)
///   5. Question section echoed byte for byte (RFC 1035 §4.1.2) — skipped for
///      the QDCOUNT == 0 error answer of check 4, which has nothing to echo
///
/// @return  std::expected<void, DnsErrorInfo> — success or a PARSE error.
namespace dns::Validator {
[[nodiscard]] std::expected<void, domain::DnsErrorInfo> validate_response(std::span<const std::uint8_t> request,
                                                                          std::span<const std::uint8_t> response);
}  // namespace dns::Validator

#endif  // YADDNSC_INFRASTRUCTURE_DNS_VALIDATOR_H
