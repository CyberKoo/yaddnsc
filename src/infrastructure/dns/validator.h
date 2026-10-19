#ifndef YADDNSC_INFRASTRUCTURE_DNS_VALIDATOR_H
#define YADDNSC_INFRASTRUCTURE_DNS_VALIDATOR_H

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>

#include "domain/error/dns_error_info.h"
#include "infrastructure/dns/types.h"

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

/// Narrow a decoded message to the IN answers that answer `host`, and decide
/// whether what is left is a usable answer at all.
///
/// Walks the in-answer CNAME chain (RFC 1034 §3.6.2) to the final owner, drops
/// foreign owners and classes, and reports the endpoint's answer as unusable
/// when `type_filter` yields no record and the reply is CNAME-only or a
/// referral without SOA negative evidence — there is no recursive referral
/// chasing. This is the answer-semantics verdict, so it lives beside the
/// request/response match rather than in the wire decoder.
///
/// An empty `host` disables owner filtering altogether — the diagnostic mode
/// that hands back every answer of the packet.
///
/// @throws DnsLookupException (PARSE) on malformed or conflicting CNAME RDATA,
///         on an overlong or cyclic chain, and on an unusable answer.
///         Allocation failure may propagate.
void select_answers(ParsedMessage& message, std::span<const std::uint8_t> wire, const std::string& host,
                    std::optional<RecordType> type_filter);
}  // namespace dns::Validator

#endif  // YADDNSC_INFRASTRUCTURE_DNS_VALIDATOR_H
