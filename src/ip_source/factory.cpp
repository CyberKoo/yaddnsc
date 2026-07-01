//
// Created by Kotarou on 2026/7/1.
//

#include "factory.h"

#include <stdexcept>

#include "http.h"
#include "iface.h"
#include "dns/types.h"
#include "config/config.h"
#include "fmt.hpp"

std::unique_ptr<IpSourceBase> IpSourceFactory::create(const Config::subdomain_config &cfg) {
    auto af = DNS::dns2ip(cfg.type);

    switch (cfg.ip_source) {
        case Config::ip_source_type::INTERFACE:
            return std::make_unique<InterfaceIpSource>(cfg.interface, af);

        case Config::ip_source_type::HTTP:
            return std::make_unique<HttpIpSource>(cfg.ip_source_param, af, cfg.interface);
    }

    throw std::runtime_error(
        fmt::format("Unsupported IP source type: {}", static_cast<int>(cfg.ip_source))
    );
}
