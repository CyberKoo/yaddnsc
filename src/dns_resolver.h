//
// Created by Kotarou on 2026/6/17.
//

#ifndef YADDNSC_DNS_RESOLVER_H
#define YADDNSC_DNS_RESOLVER_H

#include <vector>
#include <memory>
#include <optional>

#include "type.h"

class DnsResolver {
public:
    explicit DnsResolver();

    explicit DnsResolver(std::optional<dns_server> server);
    ~DnsResolver();

    DnsResolver(const DnsResolver &) = delete;
    DnsResolver &operator=(const DnsResolver &) = delete;
    DnsResolver(DnsResolver &&) = delete;
    DnsResolver &operator=(DnsResolver &&) = delete;

    [[nodiscard]] std::vector<unsigned char> query(const std::string &host, dns_record_type type) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif //YADDNSC_DNS_RESOLVER_H
