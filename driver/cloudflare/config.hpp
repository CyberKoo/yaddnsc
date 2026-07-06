//
// Created by Kotarou on 2026/6/20.
//

#ifndef YADDNSC_DRV_CLOUDFLARE_CONFIG_HPP
#define YADDNSC_DRV_CLOUDFLARE_CONFIG_HPP

#include <string>
#include <glaze/glaze.hpp>

/// Cloudflare API driver configuration parameters.
struct CloudflareParams {
    std::string zone_id;               ///< Cloudflare Zone ID
    std::string record_id;             ///< DNS Record ID to update
    std::string token;                 ///< Cloudflare API token
    std::optional<int> ttl{30};        ///< DNS record TTL in seconds (default: 30, auto)
    std::optional<bool> proxied{false}; ///< Whether the record is proxied through Cloudflare
};

/// Cloudflare API request body for DNS record updates.
struct CloudflareRequestBody {
    std::string type;    ///< DNS record type (A, AAAA, TXT, etc.)
    std::string name;    ///< Full domain name
    std::string content; ///< Record value (IP address, etc.)
    int ttl;             ///< Time-to-live in seconds
    bool proxied;        ///< Whether proxied through Cloudflare
};

template<>
struct glz::meta<CloudflareParams> {
    using T = CloudflareParams;
    static constexpr auto value = object(
        "zone_id", &T::zone_id,
        "record_id", &T::record_id,
        "token", &T::token,
        "ttl", &T::ttl,
        "proxied", &T::proxied
    );
};

template<>
struct glz::meta<CloudflareRequestBody> {
    using T = CloudflareRequestBody;
    static constexpr auto value = object(
        "type", &T::type,
        "name", &T::name,
        "content", &T::content,
        "ttl", &T::ttl,
        "proxied", &T::proxied
    );
};

#endif // YADDNSC_DRV_CLOUDFLARE_CONFIG_HPP
