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
    /// Dispatches to the system res_mkquery (libresolv) when available;
    /// falls back to the built-in builder (QueryBuilder) with a one-time
    /// warning if libresolv fails at runtime.
    ///
    /// When YADDNSC_USE_NATIVE_DNS=1, the system path is skipped entirely
    /// and the native builder is always used.
    ///
    /// @param host  The domain name to query (e.g. "example.com").
    /// @param type  The DNS record type (e.g. RecordType::A, RecordType::AAAA).
    /// @return      A buffer containing the raw DNS query packet bytes.
    /// @throws DnsLookupException  If the query packet cannot be constructed.
    [[nodiscard]] std::vector<std::uint8_t> mkquery(const std::string &host, RecordType type);

    /// Build a raw DNS query packet via the built-in QueryBuilder.
    ///
    /// Self-contained implementation that does not depend on libresolv.
    /// Used directly by the native ClassicResolver, and as the fallback
    /// path in mkquery().
    ///
    /// @param host  The domain name to query.
    /// @param type  The DNS record type (e.g. RecordType::A, RecordType::AAAA).
    /// @return      A buffer containing the raw DNS query packet bytes.
    [[nodiscard]] std::vector<std::uint8_t> mkquery_native(const std::string &host, RecordType type);

    /// Build a raw DNS query packet via libresolv's res_mkquery().
    ///
    /// @deprecated  Will be removed before the 1.0.0 release.
    ///              Use mkquery_native() instead.
    ///
    /// @param host  The domain name to query.
    /// @param type  The DNS record type (e.g. RecordType::A, RecordType::AAAA).
    /// @return      A buffer containing the raw DNS query packet bytes.
    /// @throws DnsLookupException  If res_mkquery fails.
    [[nodiscard, deprecated("Use mkquery_native() instead; will be removed before 1.0.0")]]
    std::vector<std::uint8_t> mkquery_system(const std::string &host, RecordType type);
} // namespace DNS

#endif  // YADDNSC_DNS_WIRE_QUERY_H
