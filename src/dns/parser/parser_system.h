//
// Created by Kotarou on 2026/6/17.
//

#ifndef YADDNSC_DNS_PARSER_SYSTEM_H
#define YADDNSC_DNS_PARSER_SYSTEM_H

#include <string>
#include <vector>
#include <cstdint>
#include <string_view>

#include <arpa/nameser.h>

namespace DNS {

/// Parser for raw DNS response packets (wire format, RFC 1035).
///
/// Parses DNS answer records from a raw packet buffer returned by
/// a resolver query.  Supports A, AAAA, TXT, MX, CNAME, and other
/// common record types.
///
/// @note Uses libresolv (ns_initparse / ns_parserr) internally.
///       For a self-contained alternative without libresolv,
///       see DnsParser (<dns/parser/parser_native.h>).
class DnsParser {
public:
    using data_type = std::uint8_t;

    /// Construct a parser from a raw DNS response buffer.
    /// @param data  Pointer to the raw packet bytes.
    /// @param size  Total size of the packet in bytes.
    explicit DnsParser(const data_type *data, size_t size);

    /// Return the number of answer records in the parsed response.
    [[nodiscard]] size_t record_count() const noexcept;

    /// Parse a single answer record at the given index.
    /// @param index  Zero-based index into the answer section.
    /// @return       The record value as a string (IP, hostname, text, etc.).
    [[nodiscard]] std::string parse_record(size_t index) const;

    /// Convenience: parse all answer records and return as a vector.
    ///
    /// This is the preferred entry point for most callers.
    ///
    /// @param data  Pointer to the raw packet bytes.
    /// @param size  Total size of the packet in bytes.
    /// @param host  Optional hostname for sanity checking (CNAME chain detection).
    /// @return      List of parsed record values.
    [[nodiscard]] static std::vector<std::string>
    parse_all(const data_type *data, size_t size, const std::string &host = {});

private:
    static std::string parse_a_record(const data_type *);

    static std::string parse_aaaa_record(const data_type *);

    static std::string parse_txt_record(const data_type *, int);

    static std::string parse_domain_name_record(const data_type *, const data_type *, const data_type *);

    static std::string parse_mx_record(const data_type *, const data_type *, const data_type *);

    mutable ns_msg message_{};
};

} // namespace DNS

#endif // YADDNSC_DNS_PARSER_SYSTEM_H
