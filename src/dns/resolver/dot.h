//
// Created by Kotarou on 2026/6/29.
//

#ifndef YADDNSC_DNS_DOT_H
#define YADDNSC_DNS_DOT_H

#include <expected>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

#include "base.h"
#include "dns/dns_error_info.h"

/// DotResolver — DNS-over-TLS (RFC 7858) resolver.
///
/// Uses a raw TLS socket (via OpenSSL) to send DNS queries to a DNS-over-TLS
/// server on port 853 (default).  DNS messages are framed with a 2-byte
/// big-endian length prefix as specified in RFC 7858 §3.3.
///
/// Input:  Server hostname/IP and port (default 853).
/// Output: Raw DNS response bytes (wire format), ready for DnsRecordParser.
///
/// @attention Currently, when cancel_fd is signalled, the in-flight query is
///            aborted with a CANCELLED error (via poll() on both the TLS socket
///            and the cancel fd).  This is a best-effort mechanism — the query
///            may have already been sent and the server may still process it.
///
/// @note Thread-safe: query() acquires an internal mutex around the persistent
///       TLS connection (OpenSSL BIO).  Distinct DotResolver objects are
///       independent.
class DotResolver final : public ResolverBase {
public:
    /// Construct with server address and optional port.
    /// @param server  Server hostname or IP address.
    /// @param port    TLS port (default: 853).
    /// @param label   Display label (e.g. "dot.pub:853" or "tls://dot.pub"), used in log/error messages.
    explicit DotResolver(std::string server, std::uint16_t port, std::string label);

    ~DotResolver() override;

    [[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsErrorInfo>
    query(const std::string &host, RecordKind type,
          const Utils::CancellationToken &cancel_token) const override;

    [[nodiscard]] std::string_view get_type() const noexcept override { return TYPE; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    static constexpr std::string_view TYPE = "DNS-Over-TLS";
};

#endif // YADDNSC_DNS_DOT_H
