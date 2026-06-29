//
// Created by Kotarou on 2026/6/29.
//

#ifndef YADDNSC_DNS_MKQUERY_H
#define YADDNSC_DNS_MKQUERY_H

#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// dns_mkquery — build a raw DNS query packet (wire format, RFC 1035).
//
// Dispatches to either the system res_mkquery or the manual builder
// based on the CMake option DNS_MKQUERY_USE_MANUAL.
//
// Parameters:
//   host    — The domain name to query (e.g. "example.com").
//   ns_type — The DNS record type as an <arpa/nameser.h> ns_t_* constant
//             (e.g. ns_t_a, ns_t_aaaa, ns_t_txt).
//
// Returns:
//   A buffer containing the raw DNS query packet bytes.
//
// Throws:
//   DnsLookupException if the query packet cannot be constructed.
// ---------------------------------------------------------------------------
[[nodiscard]] std::vector<uint8_t> dns_mkquery(const std::string &host, int ns_type);

// ---------------------------------------------------------------------------
// dns_mkquery_manual — build a raw DNS query packet without libresolv.
//
// A self-contained implementation that constructs the DNS wire-format query
// packet manually, without relying on res_mkquery() or any system resolver
// library.  Useful when libresolv is unavailable or when you want full
// control over the packet contents (e.g. no EDNS0 interference from
// /etc/resolv.conf).
//
// Parameters and return are identical to dns_mkquery().
// ---------------------------------------------------------------------------
[[nodiscard]] std::vector<uint8_t> dns_mkquery_manual(const std::string &host, int ns_type);

// ---------------------------------------------------------------------------
// dns_mkquery_system — build a raw DNS query packet via libresolv.
//
// Delegates packet construction to the system's res_mkquery(), which handles
// label encoding, random transaction IDs, and may append EDNS0 records based
// on the system resolver configuration (/etc/resolv.conf).
//
// Parameters and return are identical to dns_mkquery().
// ---------------------------------------------------------------------------
[[nodiscard]] std::vector<uint8_t> dns_mkquery_system(const std::string &host, int ns_type);

#endif // YADDNSC_DNS_MKQUERY_H
