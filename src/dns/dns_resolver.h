//
// Created by Kotarou on 2026/6/17.
//

#ifndef YADDNSC_DNS_DNS_RESOLVER_H
#define YADDNSC_DNS_DNS_RESOLVER_H

#include <memory>
#include <string>
#include <optional>

#include "resolver_base.h"
#include "type.h"

class DnsResolver final : public ResolverBase {
public:
    explicit DnsResolver();

    explicit DnsResolver(std::optional<dns_server> server);

    ~DnsResolver() override;

    [[nodiscard]] std::vector<uint8_t> query(const std::string &host, dns_type type) const override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif //YADDNSC_DNS_DNS_RESOLVER_H
