//
// Created by Kotarou on 2026/7/13.
//

#ifndef YADDNSC_DRV_VULTR_CONFIG_HPP
#define YADDNSC_DRV_VULTR_CONFIG_HPP

#include <string>
#include <optional>
#include <glaze/glaze.hpp>

/// Vultr API v2 driver configuration parameters.
struct VultrParams {
    std::string api_key;                 ///< Vultr API key (Bearer token)
    std::string record_id;               ///< DNS record ID to update
    std::optional<int> ttl;              ///< TTL in seconds (optional)
};

/// Vultr DNS record update request body (PATCH).
struct VultrRequestBody {
    std::string name;    ///< Subdomain name
    std::string data;    ///< Record value (IP address)
    std::optional<int> ttl;  ///< TTL in seconds
};

template<>
struct glz::meta<VultrParams> {
    using T = VultrParams;
    static constexpr auto value = object(
        "api_key", &T::api_key,
        "record_id", &T::record_id,
        "ttl", &T::ttl
    );
};

template<>
struct glz::meta<VultrRequestBody> {
    using T = VultrRequestBody;
    static constexpr auto value = object(
        "name", &T::name,
        "data", &T::data,
        "ttl", &T::ttl
    );
};

#endif // YADDNSC_DRV_VULTR_CONFIG_HPP
