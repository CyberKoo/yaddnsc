//
// Created by Kotarou on 2026/7/13.
//

#ifndef YADDNSC_DRV_LINODE_CONFIG_HPP
#define YADDNSC_DRV_LINODE_CONFIG_HPP

#include <string>
#include <optional>
#include <glaze/glaze.hpp>

/// Linode API v4 driver configuration parameters.
struct LinodeParams {
    std::string token;                   ///< Linode Personal Access Token
    std::string domain_id;               ///< Linode Domain ID
    std::string record_id;               ///< DNS record ID to update
    std::optional<int> ttl_sec;          ///< TTL in seconds (optional)
};

/// Linode DNS record update request body (PUT).
struct LinodeRequestBody {
    std::string name;                   ///< Subdomain name
    std::string target;                 ///< Record value (IP address)
    std::optional<int> ttl_sec;         ///< TTL in seconds
};

template<>
struct glz::meta<LinodeParams> {
    using T = LinodeParams;
    static constexpr auto value = object(
        "token", &T::token,
        "domain_id", &T::domain_id,
        "record_id", &T::record_id,
        "ttl_sec", &T::ttl_sec
    );
};

template<>
struct glz::meta<LinodeRequestBody> {
    using T = LinodeRequestBody;
    static constexpr auto value = object(
        "name", &T::name,
        "target", &T::target,
        "ttl_sec", &T::ttl_sec
    );
};

#endif // YADDNSC_DRV_LINODE_CONFIG_HPP
