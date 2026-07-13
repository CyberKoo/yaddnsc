//
// Created by Kotarou on 2026/7/13.
//

#ifndef YADDNSC_DRV_GODADDY_CONFIG_HPP
#define YADDNSC_DRV_GODADDY_CONFIG_HPP

#include <string>
#include <optional>
#include <glaze/glaze.hpp>

/// GoDaddy API driver configuration parameters.
struct GoDaddyParams {
    std::string key;                    ///< GoDaddy API key (SSO key prefix)
    std::string secret;                 ///< GoDaddy API secret (SSO key suffix)
    std::optional<int> ttl{600};        ///< DNS record TTL in seconds (default: 600)
};

template<>
struct glz::meta<GoDaddyParams> {
    using T = GoDaddyParams;
    static constexpr auto value = object(
        "key", &T::key,
        "secret", &T::secret,
        "ttl", &T::ttl
    );
};

#endif // YADDNSC_DRV_GODADDY_CONFIG_HPP
