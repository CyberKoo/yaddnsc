//
// Created by Kotarou on 2026/6/29.
//

#ifndef YADDNSC_DNS_DOT_H
#define YADDNSC_DNS_DOT_H

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

#include "base.h"

// ---------------------------------------------------------------------------
// DotResolver — DNS-over-TLS (RFC 7858) resolver.
//
// Uses a raw TLS socket (via OpenSSL) to send DNS queries to a DNS-over-TLS
// server on port 853 (default).  DNS messages are framed with a 2-byte
// big-endian length prefix as specified in RFC 7858 §3.3.
//
// Input:  Server hostname/IP and port (default 853).
// Output: Raw DNS response bytes (wire format), ready for DnsRecordParser.
// ---------------------------------------------------------------------------
class DotResolver final : public ResolverBase {
public:
    explicit DotResolver(std::string server, uint16_t port = 853);

    ~DotResolver() override;

    [[nodiscard]] std::vector<uint8_t> query(const std::string &host, DNS::Type type) const override;

    [[nodiscard]] std::string_view get_type() const noexcept override { return TYPE; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    static constexpr std::string_view TYPE = "DNS-Over-TLS";
};

#endif // YADDNSC_DNS_DOT_H
