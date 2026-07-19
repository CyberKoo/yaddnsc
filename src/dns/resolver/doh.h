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

class TlsConnectionBase;

/// DohResolver — DNS-over-HTTPS (RFC 8484) resolver.
///
/// Uses a TLS connection (via TlsConnectionBase) to send DNS queries to a
/// DNS-over-HTTPS server.  Queries are sent as HTTP POST requests with
/// Content-Type: application/dns-message and responses are parsed with
/// picohttpparser.  Supports cancellation via CancellationToken.
///
/// @note Thread-safe: query() acquires an internal mutex around the
///       persistent TLS connection.
class DohResolver final : public ResolverBase {
public:
    /// Construct with server hostname, port, and URL path.
    /// @param host    DoH server hostname or IP.
    /// @param port    TLS port (typically 443).
    /// @param path    URL path (e.g. "/dns-query").
    /// @param label   Display label (e.g. "dns.google:443" or "https://dns.google"), used in log/error messages.
    explicit DohResolver(std::string host, std::uint16_t port, std::string path, std::string label);

    /// Testing constructor: inject a mock TlsConnectionBase.
    /// @param host    DoH server hostname or IP.
    /// @param port    TLS port.
    /// @param path    URL path.
    /// @param label   Display label.
    /// @param conn    Mock TLS connection (takes ownership).
    DohResolver(std::string host, std::uint16_t port, std::string path, std::string label,
                std::unique_ptr<TlsConnectionBase> conn);

    ~DohResolver() override;

    [[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsErrorInfo>
    query(const std::string &host, RecordKind type,
          const Utils::CancellationToken &cancel_token) const override;

    [[nodiscard]] std::string_view get_type() const noexcept override { return TYPE; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    static constexpr std::string_view TYPE = "DNS-Over-HTTPS";
};

#endif  // YADDNSC_DNS_DOH_H
