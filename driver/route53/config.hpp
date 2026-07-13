//
// Created by Kotarou on 2026/7/13.
//

#ifndef YADDNSC_DRV_ROUTE53_CONFIG_HPP
#define YADDNSC_DRV_ROUTE53_CONFIG_HPP

#include <string>
#include <optional>
#include <glaze/glaze.hpp>

/// AWS Route 53 driver configuration parameters.
///
/// access_key_id and secret_access_key are the AWS IAM credentials used
/// for SigV4 request signing. The IAM user must have permission to call
/// ChangeResourceRecordSets on the specified hosted zone.
struct Route53Params {
    std::string access_key_id;         ///< AWS access key ID
    std::string secret_access_key;     ///< AWS secret access key
    std::string hosted_zone_id;        ///< Route 53 hosted zone ID (e.g. Z3M79L5CQABCDE)
    std::string region;                ///< AWS region (e.g. "us-east-1")
    std::string record_name;           ///< DNS record name (subdomain to update)
    std::optional<int> ttl;            ///< TTL in seconds (optional, default: 300)
};

template<>
struct glz::meta<Route53Params> {
    using T = Route53Params;
    static constexpr auto value = object(
        "access_key_id", &T::access_key_id,
        "secret_access_key", &T::secret_access_key,
        "hosted_zone_id", &T::hosted_zone_id,
        "region", &T::region,
        "record_name", &T::record_name,
        "ttl", &T::ttl
    );
};

#endif // YADDNSC_DRV_ROUTE53_CONFIG_HPP
