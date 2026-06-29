//
// Created by Kotarou on 2026/6/28.
//

#ifndef YADDNSC_DNS_DOH_H
#define YADDNSC_DNS_DOH_H

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

#include "base.h"
#include "type.h"

class HttpClient;

// ---------------------------------------------------------------------------
// DohResolver — DNS-over-HTTPS (RFC 8484) resolver.
//
// Uses the existing TransientHttpClient wrapper (not cpp-httplib directly) to send
// DNS queries as HTTPS POST requests with Content-Type: application/dns-message
// to the well-known /dns-query endpoint.
//
// Input:  DnsServer{ip_address, port} — same config struct used by ClassicResolver.
//         The HTTPS URL is derived as  https://{ip_address}/dns-query
// Output: Raw DNS response bytes (wire format), ready for DnsRecordParser.
// ---------------------------------------------------------------------------
class DohResolver final : public ResolverBase {
public:
    explicit DohResolver(std::unique_ptr<HttpClient> http_client, std::string server);

    ~DohResolver() override;

    [[nodiscard]] std::vector<uint8_t> query(const std::string &host, dns_type type) const override;

    [[nodiscard]] std::string_view get_type() const noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif // YADDNSC_DNS_DOH_H
