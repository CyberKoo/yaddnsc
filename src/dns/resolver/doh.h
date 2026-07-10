//
// Created by Kotarou on 2026/6/28.
//

#ifndef YADDNSC_DNS_DOH_H
#define YADDNSC_DNS_DOH_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <expected>

#include "base.h"
#include "dns/dns_error_info.h"

/// DohResolver — DNS-over-HTTPS (RFC 8484) resolver.
///
/// Uses a raw TLS socket (via OpenSSL BIO) to send DNS queries to a
/// DNS-over-HTTPS server.  Queries are sent as HTTP POST requests with
/// Content-Type: application/dns-message and responses are parsed with
/// picohttpparser.  Supports cancellation via cancel_fd.
///
/// @note Thread-safe: query() acquires an internal mutex around the
///       persistent TLS connection.
class DohResolver final : public ResolverBase {
public:
    /// Construct with server hostname, port, and URL path.
    /// @param host  DoH server hostname or IP.
    /// @param port  TLS port (typically 443).
    /// @param path  URL path (e.g. "/dns-query").
    explicit DohResolver(std::string host, std::uint16_t port, std::string path);

    ~DohResolver() override;

    [[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsErrorInfo>
    query(const std::string &host, RecordKind type, int cancel_fd = -1) const override;

    [[nodiscard]] std::string_view get_type() const noexcept override { return TYPE; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    static constexpr std::string_view TYPE = "DNS-Over-HTTPS";
};

#endif  // YADDNSC_DNS_DOH_H
