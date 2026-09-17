//
// Created by Kotarou on 2026/6/17.
//

#ifndef YADDNSC_DNS_CLASSIC_H
#define YADDNSC_DNS_CLASSIC_H

#include <expected>
#include <cstdint>
#include <memory>
#include <string>

#include "support/exception.h"
#include "domain/dns/record_kind.h"
#include "domain/config/dns_config.h"
#include "domain/error/dns_error_info.h"
#include "infrastructure/dns/resolver/base.h"

/// ClassicResolver — traditional UDP/TCP DNS resolver.
///
/// Queries a DNS server via the traditional UDP/TCP protocol, using the
/// built-in self-contained transport (no libresolv).
///
/// Always requires an explicit DNS server — no default constructor.
class ClassicResolver final : public ResolverBase {
public:
    /// Construct with a DNS server.
    /// @param server  DNS server address and port.
    /// @param token   Cancellation token, bound for the lifetime of the resolver.
    explicit ClassicResolver(Config::DnsServer server, Utils::CancellationToken token);

    ~ClassicResolver() override;

    [[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsErrorInfo>
    query(const std::string &host, RecordKind type) const override;

    [[nodiscard]] std::string_view get_type() const noexcept override { return TYPE; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    static constexpr std::string_view TYPE = "Classic";
};

#endif // YADDNSC_DNS_CLASSIC_H
