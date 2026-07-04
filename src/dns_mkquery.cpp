//
// Created by Kotarou on 2026/6/29.
//

#include "dns_mkquery.h"

#include <cerrno>
#include <cstring>
#include <mutex>
#include <random>

#include <resolv.h>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "exception/dns_lookup_exception.h"

// ===========================================================================
//  dns_mkquery  --  dispatch
// ===========================================================================

std::vector<uint8_t> dns_mkquery(const std::string &host, int ns_type) {
#ifdef HAVE_RES_MKQUERY
    return dns_mkquery_system(host, ns_type);
#else
    return dns_mkquery_manual(host, ns_type);
#endif
}

// ===========================================================================
//  dns_mkquery_system  --  via libresolv
// ===========================================================================

std::vector<uint8_t> dns_mkquery_system(const std::string &host, int ns_type) {
    // res_mkquery is not guaranteed to be thread-safe on all platforms
    static std::mutex mtx;
    std::lock_guard lock(mtx);

    constexpr size_t INITIAL_SIZE = 512;
    uint8_t buf[INITIAL_SIZE];

    const int len = res_mkquery(ns_o_query, host.c_str(), ns_c_in, ns_type,
                                nullptr, 0, nullptr, buf, static_cast<int>(INITIAL_SIZE));
    if (len < 0) {
        throw DnsLookupException(
            fmt::format(R"(res_mkquery failed for "{}" (type {}): {})", host, ns_type, strerror(errno)),
            dns_lookup_error_type::UNKNOWN);
    }

    SPDLOG_TRACE(R"(dns_mkquery_system: built {} bytes for "{}" (type {}))", len, host, ns_type);
    return {buf, buf + static_cast<size_t>(len)};
}

// ===========================================================================
//  dns_mkquery_manual  --  self-contained implementation (RFC 1035)
// ===========================================================================

namespace {

    void encode_dns_name(std::vector<uint8_t> &buf, const std::string &name) {
        size_t start = 0;
        while (start < name.size()) {
            auto dot = name.find('.', start);
            if (dot == std::string::npos)
                dot = name.size();
            auto len = static_cast<uint8_t>(dot - start);
            buf.push_back(len);
            for (size_t i = start; i < dot; ++i)
                buf.push_back(static_cast<uint8_t>(name[i]));
            start = dot + 1;
        }
        buf.push_back(0);
    }

    uint16_t random_id() {
        static std::mt19937 rng{std::random_device{}()};
        static std::uniform_int_distribution<uint16_t> dist;
        static std::mutex rng_mtx;
        std::lock_guard lock(rng_mtx);
        return dist(rng);
    }

} // anonymous namespace

std::vector<uint8_t> dns_mkquery_manual(const std::string &host, int ns_type) {
    std::vector<uint8_t> buf;
    buf.reserve(512);

    const auto id = random_id();
    buf.push_back(static_cast<uint8_t>(id >> 8));
    buf.push_back(static_cast<uint8_t>(id & 0xFF));

    // Flags: standard query with recursion desired (0x0100)
    buf.push_back(0x01);
    buf.push_back(0x00);

    // QDCOUNT = 1
    buf.push_back(0x00);
    buf.push_back(0x01);

    // ANCOUNT, NSCOUNT, ARCOUNT = 0
    for (int i = 0; i < 6; ++i)
        buf.push_back(0x00);

    // Question section
    encode_dns_name(buf, host);

    // QTYPE (2 bytes)
    buf.push_back(static_cast<uint8_t>((ns_type >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(ns_type & 0xFF));

    // QCLASS = IN (0x0001)
    buf.push_back(0x00);
    buf.push_back(0x01);

    SPDLOG_TRACE(R"(dns_mkquery_manual: built {} bytes for "{}" (type {}))", buf.size(), host, ns_type);
    return buf;
}
