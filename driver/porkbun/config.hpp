//
// Created by Kotarou on 2026/7/13.
//

#ifndef YADDNSC_DRV_PORKBUN_CONFIG_HPP
#define YADDNSC_DRV_PORKBUN_CONFIG_HPP

#include <string>
#include <optional>
#include <glaze/glaze.hpp>

/// Porkbun API v3 driver configuration parameters.
struct PorkbunParams {
    std::string api_key;              ///< Porkbun API key
    std::string secret_api_key;       ///< Porkbun Secret API key
    std::optional<int> ttl;           ///< TTL in seconds (optional, default: account minimum)
};

/// Porkbun DNS record update request body (POST).
struct PorkbunRequestBody {
    std::string apikey;                ///< API key (in body as alternative to header)
    std::string secretapikey;          ///< Secret API key (in body as alternative to header)
    std::string content;               ///< Record value (IP address)
    std::optional<int> ttl;            ///< TTL in seconds (optional)
};

template<>
struct glz::meta<PorkbunParams> {
    using T = PorkbunParams;
    static constexpr auto value = object(
        "api_key", &T::api_key,
        "secret_api_key", &T::secret_api_key,
        "ttl", &T::ttl
    );
};

template<>
struct glz::meta<PorkbunRequestBody> {
    using T = PorkbunRequestBody;
    static constexpr auto value = object(
        "apikey", &T::apikey,
        "secretapikey", &T::secretapikey,
        "content", &T::content,
        "ttl", &T::ttl
    );
};

#endif // YADDNSC_DRV_PORKBUN_CONFIG_HPP
