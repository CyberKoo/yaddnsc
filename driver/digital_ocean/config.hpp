//
// Created by Kotarou on 2026/6/20.
//

#ifndef YADDNSC_DRV_DIGITALOCEAN_CONFIG_HPP
#define YADDNSC_DRV_DIGITALOCEAN_CONFIG_HPP

#include <string>
#include <glaze/glaze.hpp>

/// DigitalOcean API driver configuration parameters.
struct DigitalOceanParams {
    std::string record_id; ///< DigitalOcean DNS record ID
    std::string token;     ///< DigitalOcean personal access token
};

/// DigitalOcean API request body for DNS record updates.
struct DigitalOceanBody {
    std::string data; ///< Record value (IP address)
};

template<>
struct glz::meta<DigitalOceanParams> {
    using T = DigitalOceanParams;
    static constexpr auto value = object(
        "record_id", &T::record_id,
        "token", &T::token
    );
};

template<>
struct glz::meta<DigitalOceanBody> {
    using T = DigitalOceanBody;
    static constexpr auto value = object(
        "data", &T::data
    );
};

#endif // YADDNSC_DRV_DIGITALOCEAN_CONFIG_HPP
