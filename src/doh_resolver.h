//
// Created by Kotarou on 2026/6/28.
//

#ifndef YADDNSC_DOH_RESOLVER_H
#define YADDNSC_DOH_RESOLVER_H

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

// DNS-over-HTTPS (RFC 8484) resolver.
// Sends DNS wire-format queries via HTTPS POST to a DoH server URL.
class DohResolver {
public:
    explicit DohResolver(std::string server_url);
    ~DohResolver();

    // Returns raw DNS response bytes
    std::vector<uint8_t> query(const std::string &host, int ns_type);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif // YADDNSC_DOH_RESOLVER_H
