//
// Created by Kotarou on 2026/6/17.
//

#ifndef YADDNSC_DNS_PARSER_SYSTEM_H
#define YADDNSC_DNS_PARSER_SYSTEM_H

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// ---------------------------------------------------------------------------
// <arpa/nameser.h> defines preprocessor macros NOERROR, SERVFAIL, NXDOMAIN,
// and REFUSED that collide with the DNS::Rcode enum values in dns/types.h.
//
// This header is therefore included BEFORE dns/types.h so that the macros
// can be #undef'd before the enum is seen.  If you move this include, the
// Rcode enum will silently produce wrong code (macro-substituted values).
// ---------------------------------------------------------------------------
#include <arpa/nameser.h>

// Undefine conflicting macros BEFORE including dns/types.h.
#ifdef NOERROR
#  undef NOERROR
#endif
#ifdef NXDOMAIN
#  undef NXDOMAIN
#endif
#ifdef SERVFAIL
#  undef SERVFAIL
#endif
#ifdef REFUSED
#  undef REFUSED
#endif
#ifdef NOERROR
#  undef NOERROR
#endif
#ifdef NXDOMAIN
#  undef NXDOMAIN
#endif
#ifdef SERVFAIL
#  undef SERVFAIL
#endif
#ifdef REFUSED
#  undef REFUSED
#endif

#include "dns/types.h"

namespace DNS
{

/// Parser for raw DNS response packets (wire format, RFC 1035).
///
/// Parses DNS answer records from a raw packet buffer returned by
/// a resolver query.  Supports A, AAAA, TXT, MX, CNAME, and other
/// common record types.
///
/// @note Stable default (libresolv).  See parser_native for the experimental
///       self-contained alternative (no libresolv).
class RecordParser
{
public:
  /// Construct a parser from a raw DNS response buffer.
  /// @param data  Span covering the raw packet bytes.
  explicit RecordParser(std::span<const std::uint8_t> data);

  /// Return the number of answer records in the parsed response.
  [[nodiscard]] size_t record_count() const noexcept;

  /// Parse a single answer record at the given index.
  /// @param index  Zero-based index into the answer section.
  /// @return       The record value as a string (IP, hostname, text, etc.).
  [[nodiscard]] std::string parse_record(size_t index) const;

  /// Parse a raw DNS response and return the full ParsedResponse with
  /// original ResourceRecord answers (raw RDATA, TTL, type).
  ///
  /// @param data  Span covering the raw packet bytes.
  /// @param host  Optional hostname for sanity checking (CNAME chain detection).
  /// @return      Structured result with RCODE and raw answer records.
  [[nodiscard]] static ParsedResponse parse_response(std::span<const std::uint8_t> data, const std::string& host = {});

  /// Convenience: parse all answer records and return pre-formatted
  /// string values (IPs, hostnames, text, etc.).
  ///
  /// @param data  Span covering the raw packet bytes.
  /// @param host  Optional hostname for sanity checking (CNAME chain detection).
  /// @return      Structured result with RCODE and pre-formatted record values.
  [[nodiscard]] static FormattedResponse parse_strings(std::span<const std::uint8_t> data,
                                                       const std::string& host = {});

  /// Return the RCODE from the parsed DNS header.
  [[nodiscard]] std::uint8_t rcode() const noexcept
  {
    return rcode_;
  }

private:
  [[nodiscard]] static std::string parse_a_record(std::span<const std::uint8_t> rdata);

  [[nodiscard]] static std::string parse_aaaa_record(std::span<const std::uint8_t> rdata);

  [[nodiscard]] static std::string parse_txt_record(std::span<const std::uint8_t> rdata);

  [[nodiscard]] static std::string parse_domain_name_record(std::span<const std::uint8_t> msg,
                                                            const std::uint8_t* rdata);

  [[nodiscard]] static std::string parse_mx_record(std::span<const std::uint8_t> msg, const std::uint8_t* rdata);

  /// RCODE extracted from the raw DNS header.
  std::uint8_t rcode_{0};

  mutable ns_msg message_{};
};

}  // namespace DNS

#endif  // YADDNSC_DNS_PARSER_SYSTEM_H
