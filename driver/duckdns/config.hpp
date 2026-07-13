//
// Created by Kotarou on 2026/7/13.
//

#ifndef YADDNSC_DRV_DUCKDNS_CONFIG_HPP
#define YADDNSC_DRV_DUCKDNS_CONFIG_HPP

#include <string>
#include <glaze/glaze.hpp>

/// DuckDNS API driver configuration parameters.
struct DuckDnsParams {
    std::string token;                  ///< DuckDNS API token (required)
    std::optional<bool> verbose{false}; ///< Return verbose response with extra information
};

template<>
struct glz::meta<DuckDnsParams> {
    using T = DuckDnsParams;
    static constexpr auto value = object(
        "token", &T::token,
        "verbose", &T::verbose
    );
};

#endif // YADDNSC_DRV_DUCKDNS_CONFIG_HPP
