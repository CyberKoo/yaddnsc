//
// Created by Kotarou on 2026/6/17.
//

#ifndef YADDNSC_DNS_RESOLVER_H
#define YADDNSC_DNS_RESOLVER_H

#include <vector>
#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>

#include "type.h"

class DnsResolver {
public:
    explicit DnsResolver(std::optional<dns_server> server = std::nullopt);
    ~DnsResolver();

    DnsResolver(const DnsResolver &) = delete;
    DnsResolver &operator=(const DnsResolver &) = delete;
    DnsResolver(DnsResolver &&) = delete;
    DnsResolver &operator=(DnsResolver &&) = delete;

    [[nodiscard]] std::vector<unsigned char> query(std::string_view host, dns_record_type type) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif //YADDNSC_DNS_RESOLVER_H
