//
// Created by Kotarou on 2026/7/1.
//

#include "factory.h"

#include <utility>

#include "config/config.h"

#include "address_family.h"
#include "http.h"
#include "iface.h"
#include "mdns.h"
#include "record_kind.h"

namespace {
    /// Convert RecordKind to the corresponding address family.
    /// Used to select the appropriate IP source (IPv4-only / IPv6-only).
    [[nodiscard]] constexpr AddressFamily type_to_family(RecordKind type) noexcept {
        switch (type) {
            case RecordKind::A:
                return AddressFamily::IPV4;
            case RecordKind::AAAA:
                return AddressFamily::IPV6;
            default:
                return AddressFamily::UNSPECIFIED;
        }
    }
} // anonymous namespace

// ===========================================================================
// IpSourceFactory::create — build the correct IP source from subdomain config.
// ===========================================================================

std::unique_ptr<IpSourceBase> IpSourceFactory::create(const Config::SubdomainConfig &cfg) {
    auto address_family = type_to_family(cfg.type);

    switch (cfg.ip_source) {
        case Config::IpSource::INTERFACE:
            return std::make_unique<InterfaceIpSource>(cfg.interface, address_family);

        case Config::IpSource::HTTP:
            return std::make_unique<HttpIpSource>(cfg.ip_source_param, address_family, cfg.interface);

        case Config::IpSource::MDNS:
            return std::make_unique<MdnsIpSource>(cfg.ip_source_param, cfg.type, cfg.interface);
    }

    std::unreachable();
}
