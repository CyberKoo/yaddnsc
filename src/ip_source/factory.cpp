//
// Created by Kotarou on 2026/7/1.
//

#include "factory.h"

#include <stdexcept>

#include <magic_enum/magic_enum.hpp>

#include "http.h"
#include "iface.h"
#include "mdns.h"
#include "fmt.hpp"
#include "dns/util.hpp"
#include "config/config.h"

// ===========================================================================
// IpSourceFactory::create — build the correct IP source from subdomain config.
// ===========================================================================

std::unique_ptr<IpSourceBase> IpSourceFactory::create(const Config::SubdomainConfig &cfg) {
    auto address_family = DNS::Util::type_to_family(cfg.type);

    switch (cfg.ip_source) {
        case Config::IpSource::INTERFACE:
            return std::make_unique<InterfaceIpSource>(cfg.interface, address_family);

        case Config::IpSource::HTTP:
            return std::make_unique<HttpIpSource>(cfg.ip_source_param, address_family, cfg.interface);

        case Config::IpSource::MDNS:
            return std::make_unique<MdnsIpSource>(cfg.ip_source_param, cfg.type, cfg.interface);
    }

    throw std::runtime_error(
        fmt::format("Unsupported IP source type: {}", magic_enum::enum_name(cfg.ip_source))
    );
}
