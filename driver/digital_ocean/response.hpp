//
// Created by kotarou on 2026/6/17.
//

#ifndef YADDNSC_DRV_DIGITALOCEAN_RESPONSE_H
#define YADDNSC_DRV_DIGITALOCEAN_RESPONSE_H

#include <cstdint>
#include <string>
#include <optional>

#include <glaze/glaze.hpp>

/// DigitalOcean DNS domain record as returned by the API.
/// Success response shape: { "domain_record": { ... } }
struct DigitalOceanDomainRecord {
    std::string data;                ///< Record value
    std::optional<int64_t> flags;    ///< Flags (for certain record types)
    int64_t id = 0;                  ///< Record ID
    std::string name;                ///< Domain name
    std::optional<int64_t> port;     ///< Port (for SRV records)
    std::optional<int64_t> priority; ///< Priority (for MX/SRV records)
    std::optional<std::string> tag;  ///< Tag
    int64_t ttl = 0;                 ///< TTL in seconds
    std::string type;                ///< Record type
    std::optional<int64_t> weight;   ///< Weight (for SRV records)
};

/// DigitalOcean success response wrapper.
struct DigitalOceanDomainResponse {
    DigitalOceanDomainRecord domain_record;
};

/// DigitalOcean error response.
/// Error shape: { "id": "...", "message": "..." }
struct DigitalOceanErrorResponse {
    std::string id;      ///< Error identifier
    std::string message; ///< Error description
};

template<>
struct glz::meta<DigitalOceanDomainRecord> {
    using T = DigitalOceanDomainRecord;
    static constexpr auto value = object(
        "data", &T::data,
        "flags", &T::flags,
        "id", &T::id,
        "name", &T::name,
        "port", &T::port,
        "priority", &T::priority,
        "tag", &T::tag,
        "ttl", &T::ttl,
        "type", &T::type,
        "weight", &T::weight
    );
};

template<>
struct glz::meta<DigitalOceanDomainResponse> {
    using T = DigitalOceanDomainResponse;
    static constexpr auto value = object(
        "domain_record", &T::domain_record
    );
};

template<>
struct glz::meta<DigitalOceanErrorResponse> {
    using T = DigitalOceanErrorResponse;
    static constexpr auto value = object(
        "id", &T::id,
        "message", &T::message
    );
};

#endif //YADDNSC_DRV_DIGITALOCEAN_RESPONSE_H
