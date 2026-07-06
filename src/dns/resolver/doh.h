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

class HttpClient;

/// DohResolver — DNS-over-HTTPS (RFC 8484) resolver.
///
/// Sends DNS queries as HTTPS POST requests with Content-Type:
/// application/dns-message and returns the raw DNS wire-format response.
class DohResolver final : public ResolverBase {
public:
    /// Construct with an HTTP client and a DoH server URL.
    /// @param http_client  HTTP client for sending requests.
    ///                     Should have redirect-following enabled for
    ///                     robustness against endpoint migrations.
    /// @param server       Full DoH server URL
    ///                     (e.g. "https://dns.google/dns-query").
    explicit DohResolver(std::unique_ptr<HttpClient> http_client, std::string server);

    ~DohResolver() override;

    std::vector<std::uint8_t> query(const std::string &host, DNS::Type type) const override;

    [[nodiscard]] std::string_view get_type() const noexcept override { return TYPE; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    static constexpr std::string_view TYPE = "DNS-Over-HTTPS";
};

#endif // YADDNSC_DNS_DOH_H
