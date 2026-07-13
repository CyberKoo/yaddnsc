//
// Created by Kotarou on 2026/7/13.
//

#ifndef YADDNSC_DRV_ALIBABA_CLOUD_CONFIG_HPP
#define YADDNSC_DRV_ALIBABA_CLOUD_CONFIG_HPP

#include <string>
#include <optional>
#include <glaze/glaze.hpp>

/// Alibaba Cloud DNS (Alidns) driver configuration parameters.
///
/// access_key_id and access_key_secret are the Alibaba Cloud RAM user
/// AccessKey credentials used for RPC request signing.
struct AlibabaParams {
    std::string access_key_id;          ///< Alibaba Cloud AccessKey ID
    std::string access_key_secret;      ///< Alibaba Cloud AccessKey Secret
    std::string record_id;              ///< DNS Record ID to update
    std::optional<int> ttl;             ///< TTL in seconds (optional, default: 600)
};

template<>
struct glz::meta<AlibabaParams> {
    using T = AlibabaParams;
    static constexpr auto value = object(
        "access_key_id", &T::access_key_id,
        "access_key_secret", &T::access_key_secret,
        "record_id", &T::record_id,
        "ttl", &T::ttl
    );
};

#endif // YADDNSC_DRV_ALIBABA_CLOUD_CONFIG_HPP
