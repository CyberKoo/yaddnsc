//
// Created by Kotarou on 2026/6/29.
//

#ifndef YADDNSC_MKQUERY_H
#define YADDNSC_MKQUERY_H

#include <string>
#include <vector>
#include <cstdint>

namespace DNS {
    // ---------------------------------------------------------------------------
    // mkquery — build a raw DNS query packet (wire format, RFC 1035).
    //
    // Dispatches to either the system res_mkquery or the manual builder
    // based on the CMake option YADDNSC_MANUAL_MKQUERY.
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
    [[nodiscard]] std::vector<uint8_t> mkquery(const std::string &host, int ns_type);

    // ---------------------------------------------------------------------------
    // mkquery_manual — build a raw DNS query packet without libresolv.
    //
    // A self-contained implementation that constructs the DNS wire-format query
    // packet manually, without relying on res_mkquery() or any system resolver
    // library.  Useful when libresolv is unavailable or when you want full
    // control over the packet contents (e.g. no EDNS0 interference from
    // /etc/resolv.conf).
    //
    // Parameters and return are identical to mkquery().
    // ---------------------------------------------------------------------------
    [[nodiscard]] std::vector<uint8_t> mkquery_manual(const std::string &host, int ns_type);

    // ---------------------------------------------------------------------------
    // mkquery_system — build a raw DNS query packet via libresolv.
    //
    // Delegates packet construction to the system's res_mkquery(), which handles
    // label encoding, random transaction IDs, and may append EDNS0 records based
    // on the system resolver configuration (/etc/resolv.conf).
    //
    // Parameters and return are identical to mkquery().
    // ---------------------------------------------------------------------------
    [[nodiscard]] std::vector<uint8_t> mkquery_system(const std::string &host, int ns_type);

    // ---------------------------------------------------------------------------
    // mkquery_mdns — build an mDNS query packet (RFC 6762).
    //
    // Constructs a DNS wire-format query suitable for multicast DNS.  Differs
    // from mkquery_manual in three ways:
    //   1. Transaction ID is set to 0 (RFC 6762 §18.1).
    //   2. Flags are set to 0x0000 — no RD, no RA, standard query.
    //   3. QCLASS carries the QU (unicast-response) bit (0x8001) so that the
    //      responder sends a unicast reply instead of multicasting it (RFC 6762
    //      §18.3).
    //
    // Parameters:
    //   host        — The mDNS hostname to query (e.g. "printer.local").
    //   ns_type     — The DNS record type (ns_t_a, ns_t_aaaa, etc.).
    //   unicast_rsp — When true (default), set the QU bit in QCLASS; when false,
    //                 the responder will multicast its reply.
    //
    // Returns:
    //   A buffer containing the raw mDNS query packet bytes.
    // ---------------------------------------------------------------------------
    [[nodiscard]] std::vector<uint8_t> mkquery_mdns(const std::string &host, int ns_type, bool unicast_rsp = true);
} // namespace DNS

#endif // YADDNSC_MKQUERY_H
