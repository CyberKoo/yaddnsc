//
// Created by Kotarou on 2026/6/29.
//

#include "dns_mkquery.h"

#include <arpa/nameser.h>
#include <resolv.h>

#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "dns_mkquery_config.h"
#include "exception/dns_lookup_exception.h"
#include "fmt.hpp"

// ===========================================================================
//  dns_mkquery  —  compile-time dispatch
// ===========================================================================

std::vector<uint8_t> dns_mkquery(const std::string &host, int ns_type) {
    if constexpr (YADDNSC_MANUAL_MKQUERY) {
        return dns_mkquery_manual(host, ns_type);
    } else {
        return dns_mkquery_system(host, ns_type);
    }
}

// ===========================================================================
//  dns_mkquery_system  —  system res_mkquery wrapper
// ===========================================================================

std::vector<uint8_t> dns_mkquery_system(const std::string &host, int ns_type) {
    // res_mkquery may append EDNS0 records from the system resolver config
    // (e.g. /etc/resolv.conf), so allocate a generous buffer.
    static constexpr size_t BUFFER_SIZE = 4096;
    std::vector<uint8_t> buf(BUFFER_SIZE);

    // res_mkquery reads/writes the process-global _res state (e.g. _res.id),
    // so it must be serialized.
    int len = 0;
    static std::mutex res_mutex;
    {
        std::lock_guard lock(res_mutex);
        len = res_mkquery(ns_o_query, host.c_str(), ns_c_in, ns_type, nullptr, 0, nullptr, buf.data(),
                          static_cast<int>(buf.size()));
    }

    if (len < 0) {
        throw DnsLookupException(
            fmt::format(R"(Failed to construct DNS query packet for "{}")", host),
            dns_error_type::UNKNOWN
        );
    }

    buf.resize(static_cast<size_t>(len));
    return buf;
}

// ===========================================================================
//  dns_mkquery_manual  —  hand-rolled DNS query builder (no libresolv)
// ===========================================================================

namespace {
    struct QueryWriter {
        std::vector<uint8_t> buf;

        QueryWriter() { buf.reserve(512); }

        void write_uint16(uint16_t v) {
            buf.push_back(static_cast<uint8_t>(v >> 8));
            buf.push_back(static_cast<uint8_t>(v & 0xFF));
        }

        // Encode a domain name into DNS label sequence (RFC 1035 §4.1.2).
        void encode_domain_name(const std::string &name) {
            size_t pos = 0;
            while (pos < name.size()) {
                auto dot = name.find('.', pos);
                if (dot == std::string::npos) {
                    dot = name.size();
                }
                const auto label_len = static_cast<uint8_t>(dot - pos);
                buf.push_back(label_len);
                for (size_t i = 0; i < label_len; ++i) {
                    buf.push_back(static_cast<uint8_t>(name[pos + i]));
                }
                pos = dot + 1;
            }
            buf.push_back(0); // root label (terminator)
        }

        std::vector<uint8_t> finish() { return std::move(buf); }
    };
} // anonymous namespace

std::vector<uint8_t> dns_mkquery_manual(const std::string &host, int ns_type) {
    static std::random_device rd;
    const auto id = static_cast<uint16_t>(rd() & 0xFFFF);

    QueryWriter w;
    // ---- Header (12 bytes) ----
    w.write_uint16(id); // Transaction ID
    w.write_uint16(0x0100); // Flags:  standard query (QR=0), RD=1
    w.write_uint16(1); // QDCOUNT = 1 question
    w.write_uint16(0); // ANCOUNT = 0
    w.write_uint16(0); // NSCOUNT = 0
    w.write_uint16(0); // ARCOUNT = 0

    // ---- Question ----
    w.encode_domain_name(host); // QNAME
    w.write_uint16(static_cast<uint16_t>(ns_type)); // QTYPE
    w.write_uint16(1); // QCLASS = IN (Internet)

    return w.finish();
}
