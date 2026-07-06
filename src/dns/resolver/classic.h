//
// Created by Kotarou on 2026/6/17.
//

#ifndef YADDNSC_DNS_CLASSIC_H
#define YADDNSC_DNS_CLASSIC_H

#include <cstdint>
#include <memory>
#include <string>

#include "dns_type.h"
#include "base.h"

/// ClassicResolver — traditional UDP/TCP DNS resolver.
///
/// Queries a DNS server via the traditional UDP/TCP protocol.
/// The underlying implementation is selected at compile time:
///   - YADDNSC_NATIVE_DNS=0 → system libresolv (res_nquery / res_query)
///   - YADDNSC_NATIVE_DNS=1 → built-in raw UDP/TCP (no libresolv)
///
/// Always requires an explicit DNS server — no default constructor.
class ClassicResolver final : public ResolverBase {
public:
    /// Construct with a DNS server.
    /// @param server  DNS server address and port.
    explicit ClassicResolver(DNS::Server server);

    ~ClassicResolver() override;

    [[nodiscard]] std::vector<std::uint8_t> query(const std::string &host, DNS::Type type) const override;

    [[nodiscard]] std::string_view get_type() const noexcept override { return TYPE; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    static constexpr std::string_view TYPE = "Classic";
};

#endif // YADDNSC_DNS_CLASSIC_H
