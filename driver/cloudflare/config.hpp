//
// Created by Kotarou on 2026/6/20.
//

#ifndef YADDNSC_DRV_CLOUDFLARE_CONFIG_HPP
#define YADDNSC_DRV_CLOUDFLARE_CONFIG_HPP

#include <string>
#include <glaze/glaze.hpp>

struct CloudflareParams {
    std::string zone_id;
    std::string record_id;
    std::string token;
    std::optional<int> ttl{30};
    std::optional<bool> proxied{false};
};

struct CloudflareRequestBody {
    std::string type;
    std::string name;
    std::string content;
    int ttl;
    bool proxied;
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
