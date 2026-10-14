#ifndef YADDNSC_INFRASTRUCTURE_DNS_VALIDATOR_H
#define YADDNSC_INFRASTRUCTURE_DNS_VALIDATOR_H

#include <cstdint>
#include <span>

#include <expected>

#include "domain/error/dns_error_info.h"

namespace dns {
struct Question;
}  // namespace dns

/// Validate a DNS response against its request.
///
/// Checks the local request first, then response ID, opcode, QR, question count
/// and semantic QNAME/QTYPE/QCLASS equality. Names are case-insensitive but
/// preserve label boundaries; compression encoding does not affect equality.
/// A nonzero-RCODE response may omit its question.
///
/// Only header/question bytes are needed, so a truncated RR section can still
/// be matched before transport fallback.
/// @return Success, INVALID_QUERY for an invalid local header/count, or PARSE
///         for an unusable remote response. Allocation failure may propagate.
namespace dns::Validator {
/// Reject an invalid local header/count/question before performing I/O.
/// Allocation failures propagate; input remains borrowed for this call only.
[[nodiscard]] std::expected<void, domain::DnsErrorInfo> validate_query(std::span<const std::uint8_t> request);

/// True for a matching response, false for an unrelated ID/opcode/question.
/// INVALID_QUERY is local; PARSE identifies a malformed response with matching
/// ID/opcode. Callers must check errors, not treat every failure as unrelated.
[[nodiscard]] std::expected<bool, domain::DnsErrorInfo> match_response(std::span<const std::uint8_t> request,
                                                                       std::span<const std::uint8_t> response);

/// The same comparison against an already parsed request question.
///
/// A caller that answers many packets for one query — a UDP receive loop —
/// validates and parses its request once and then pays only the response-side
/// parse per datagram. `asked` must be the question `request` carries; `request`
/// is still read for the header bytes it is compared against.
[[nodiscard]] std::expected<bool, domain::DnsErrorInfo> match_response(std::span<const std::uint8_t> request,
                                                                       const Question& asked,
                                                                       std::span<const std::uint8_t> response);
[[nodiscard]] std::expected<void, domain::DnsErrorInfo> validate_response(std::span<const std::uint8_t> request,
                                                                          std::span<const std::uint8_t> response);
}  // namespace dns::Validator

#endif  // YADDNSC_INFRASTRUCTURE_DNS_VALIDATOR_H
