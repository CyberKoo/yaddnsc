//
// Created by Kotarou on 2026/6/17.
//

#ifndef YADDNSC_DNS_CLASSIC_H
#define YADDNSC_DNS_CLASSIC_H

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "infrastructure/dns/resolver/base.h"

namespace Config {
struct DnsServer;
}  // namespace Config

namespace Utils {
class CancellationToken;
}  // namespace Utils

/// ClassicResolver — traditional UDP/TCP DNS resolver.
///
/// Queries a DNS server via the traditional UDP/TCP protocol, using the
/// built-in self-contained transport (no libresolv).
///
/// UDP runs through DNS::exchange_udp. A truncated answer falls back to TCP
/// through Transport::TcpStream, so this resolver sits above the transport
/// layer. Note that the TCP budget is per stream operation: connect, the
/// length-prefixed write, and each exact read each get a fresh budget rather
/// than sharing one clock across the whole fallback. Bootstrap, which lives
/// below the transport layer and cannot use TcpStream, has the opposite
/// contract — its TCP exchange shares a single deadline
/// (DNS::exchange_tcp).
///
/// Always requires an explicit DNS server — no default constructor.
/// Cancellation is operation-scoped: query() takes the caller's token.
class ClassicResolver final : public ResolverBase {
public:
    /// Construct with a DNS server.
    /// @param server  DNS server address and port.
    explicit ClassicResolver(Config::DnsServer server);

    ~ClassicResolver() override;

    [[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsErrorInfo> query(
        const std::string& host, RecordKind type, const Utils::CancellationToken& token) const override;

    [[nodiscard]] std::string_view get_type() const noexcept override { return TYPE; }

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
    static constexpr std::string_view TYPE = "Classic";
};

#endif  // YADDNSC_DNS_CLASSIC_H
