//
// Created by Kotarou on 2026/6/20.
//

#ifndef YADDNSC_DRV_DNSPOD_CONFIG_HPP
#define YADDNSC_DRV_DNSPOD_CONFIG_HPP

#include <optional>
#include <string>
#include <glaze/glaze.hpp>

/// DNSPod API driver configuration parameters.
struct DNSPodParams {
    std::string domain_id;                    ///< DNSPod domain ID
    std::string record_id;                    ///< DNS record ID to update
    std::string login_token;                  ///< DNSPod API login token
    std::optional<std::string> record_line;   ///< Record line (e.g. "默认", "电信")
    std::string record_line_id{"0"};          ///< Record line ID (default: "0")
    bool global{false};                       ///< Whether the record is global (load balancing)
};

template<>
struct glz::meta<DNSPodParams> {
    using T = DNSPodParams;
    static constexpr auto value = object(
        "domain_id", &T::domain_id,
        "record_id", &T::record_id,
        "login_token", &T::login_token,
        "record_line", &T::record_line,
        "record_line_id", &T::record_line_id,
        "global", &T::global
    );
};

#endif // YADDNSC_DRV_DNSPOD_CONFIG_HPP
