//
// Created by Kotarou on 2026/7/1.
//

#include "factory.h"

#include <stdexcept>

#include <magic_enum/magic_enum.hpp>

#include "http.h"
#include "iface.h"
#include "fmt.hpp"
#include "dns/types.h"
#include "config/config.h"

std::unique_ptr<IpSourceBase> IpSourceFactory::create(const Config::subdomain_config &cfg) {
    auto address_family = DNS::dns2ip(cfg.type);

    switch (cfg.ip_source) {
        case Config::ip_source_type::INTERFACE:
            return std::make_unique<InterfaceIpSource>(cfg.interface, address_family);

        case Config::ip_source_type::HTTP:
            return std::make_unique<HttpIpSource>(cfg.ip_source_param, address_family, cfg.interface);
    }

    throw std::runtime_error(
        fmt::format("Unsupported IP source type: {}", magic_enum::enum_name(cfg.ip_source))
    );
}
