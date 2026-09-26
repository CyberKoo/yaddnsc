//
// Created by Kotarou on 2026/7/1.
//

#include "factory.h"

#include <exception>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "domain/config/ip_source_kind.h"
#include "domain/config/runtime_config.h"
#include "domain/dns/record_kind.h"
#include "domain/network/address_family.h"

#include "http.h"
#include "iface.h"
#include "mdns.h"

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
}  // anonymous namespace

// ===========================================================================
// IpSourceFactory::create — build the correct IP source from subdomain config.
// ===========================================================================

/// Build the correct IP source from the subdomain configuration.
///
/// Dispatches to InterfaceIpSource, HttpIpSource, or MdnsIpSource
/// based on Config::IpSource.
/// @param cfg  The subdomain configuration record.
/// @return     A unique pointer to the concrete IP source implementation.
IpSourceFactory::Result IpSourceFactory::create(const domain::SubdomainConfig& cfg,
                                                std::vector<Config::DnsServer> bootstrap) {
    auto address_family = type_to_family(cfg.type);

    try {
        switch (cfg.ip_source) {
            case Config::IpSource::INTERFACE:
                return std::make_unique<InterfaceIpSource>(cfg.interface, address_family);

            case Config::IpSource::HTTP:
                return std::make_unique<HttpIpSource>(cfg.ip_source_param, address_family, cfg.interface,
                                                      std::move(bootstrap));

            case Config::IpSource::MDNS:
                return std::make_unique<MdnsIpSource>(cfg.ip_source_param, cfg.type, cfg.interface);
        }
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& error) {
        return std::unexpected(domain::IpSourceError{domain::IpSourceError::Code::UNAVAILABLE, error.what()});
    } catch (...) {
        return std::unexpected(
            domain::IpSourceError{domain::IpSourceError::Code::UNKNOWN, "unknown IP source construction exception"});
    }

    return std::unexpected(
        domain::IpSourceError{domain::IpSourceError::Code::UNKNOWN, "unknown IP source kind"});
}
