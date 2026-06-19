//
// Created by kotarou on 2026/6/17.
//

#ifndef YADDNSC_DRV_DIGITALOCEAN_RESPONSE_H
#define YADDNSC_DRV_DIGITALOCEAN_RESPONSE_H

#include <cstdint>
#include <string>
#include <optional>

#include <glaze/glaze.hpp>

// Success: { "domain_record": { ... } }
struct DigitalOceanDomainRecord {
    std::string data;
    std::optional<int64_t> flags;
    int64_t id = 0;
    std::string name;
    std::optional<int64_t> port;
    std::optional<int64_t> priority;
    std::optional<std::string> tag;
    int64_t ttl = 0;
    std::string type;
    std::optional<int64_t> weight;
};

struct DigitalOceanDomainResponse {
    DigitalOceanDomainRecord domain_record;
};

// Error: { "id": "...", "message": "..." }
struct DigitalOceanErrorResponse {
    std::string id;
    std::string message;
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
