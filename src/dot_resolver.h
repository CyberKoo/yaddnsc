//
// Created by Kotarou on 2026/6/29.
//

#ifndef YADDNSC_DOT_RESOLVER_H
#define YADDNSC_DOT_RESOLVER_H

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

// DNS-over-TLS (RFC 7858) resolver.
// Uses OpenSSL to establish a TLS connection and send DNS queries
// with 2-byte length-prefixed framing (RFC 7858 §3).
class DotResolver {
public:
    explicit DotResolver(std::string server, uint16_t port = 853);
    ~DotResolver();

    // Returns raw DNS response bytes
    std::vector<uint8_t> query(const std::string &host, int ns_type) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif // YADDNSC_DOT_RESOLVER_H
