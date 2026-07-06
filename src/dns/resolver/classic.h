//
// Created by Kotarou on 2026/6/17.
//

#ifndef YADDNSC_DNS_CLASSIC_H
#define YADDNSC_DNS_CLASSIC_H

#include <memory>
#include <string>
#include <optional>

#include "dns_type.h"
#include "base.h"

/// ClassicResolver — DNS resolver using the system libresolv (res_query).
///
/// Queries the system-configured DNS servers (or a custom server) via
/// the traditional res_query() API.
class ClassicResolver final : public ResolverBase {
public:
    /// Construct with system default resolver configuration.
    explicit ClassicResolver();

    /// Construct with a custom DNS server.
    explicit ClassicResolver(std::optional<DNS::Server> server);

    ~ClassicResolver() override;

    [[nodiscard]] std::vector<uint8_t> query(const std::string &host, DNS::Type type) const override;

    [[nodiscard]] std::string_view get_type() const noexcept override { return TYPE; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    static constexpr std::string_view TYPE = "Classic";
};

#endif // YADDNSC_DNS_CLASSIC_H
