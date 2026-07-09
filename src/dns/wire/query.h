//
// Created by Kotarou on 2026/6/29.
//

#ifndef YADDNSC_DNS_WIRE_QUERY_H
#define YADDNSC_DNS_WIRE_QUERY_H

#include <cstdint>
#include <string>
#include <vector>

#include "dns/types.h"

namespace DNS {
    /// Build a raw DNS query packet (wire format, RFC 1035).
    ///
    /// Dispatches to either the system res_mkquery or the manual builder
    /// based on the CMake option YADDNSC_USE_NATIVE_DNS.
    ///
    /// @param host  The domain name to query (e.g. "example.com").
    /// @param type  The DNS record type (e.g. RecordType::A, RecordType::AAAA).
    /// @return      A buffer containing the raw DNS query packet bytes.
    /// @throws DnsLookupException  If the query packet cannot be constructed.
    [[nodiscard]] std::vector<std::uint8_t> mkquery(const std::string &host, RecordType type);

    /// EXPERIMENTAL: Build a raw DNS query packet without libresolv.
    ///
    /// Self-contained implementation that avoids EDNS0 interference
    /// from /etc/resolv.conf.  Not enabled by default.
    ///
    /// @param host  The domain name to query.
    /// @param type  The DNS record type (e.g. RecordType::A, RecordType::AAAA).
    /// @return      A buffer containing the raw DNS query packet bytes.
    [[nodiscard]] std::vector<std::uint8_t> mkquery_native(const std::string &host, RecordType type);

    /// Build a raw DNS query packet via libresolv.
    ///
    /// Delegates packet construction to the system's res_mkquery(), which handles
    /// label encoding, random transaction IDs, and may append EDNS0 records based
    /// on the system resolver configuration (/etc/resolv.conf).
    ///
    /// @param host  The domain name to query.
    /// @param type  The DNS record type (e.g. RecordType::A, RecordType::AAAA).
    /// @return      A buffer containing the raw DNS query packet bytes.
    [[nodiscard]] std::vector<std::uint8_t> mkquery_system(const std::string &host, RecordType type);
} // namespace DNS

#endif  // YADDNSC_DNS_WIRE_QUERY_H
