//
// Created by kotarou on 2026/6/17.
//

#ifndef YADDNSC_DRV_DNSPOD_RESPONSE_H
#define YADDNSC_DRV_DNSPOD_RESPONSE_H

#include <cstdint>
#include <string>
#include <optional>

#include <glaze/glaze.hpp>

/// DNSPod API response status information.
struct DnsPodStatus {
    std::string code;       ///< Status code (e.g. "1" for success)
    std::string message;    ///< Status message
    std::string created_at; ///< Timestamp of the operation
};

/// DNSPod DNS record as returned by the API.
struct DnsPodRecord {
    int64_t id{};     ///< Record ID
    std::string name; ///< Domain name
    std::string value; ///< Record value
};

/// Top-level DNSPod API response.
struct DnsPodResponse {
    std::optional<DnsPodStatus> status; ///< Response status
    std::optional<DnsPodRecord> record; ///< DNS record data (present on success)
};

template<>
struct glz::meta<DnsPodStatus> {
    using T = DnsPodStatus;
    static constexpr auto value = object(
        "code", &T::code,
        "message", &T::message,
        "created_at", &T::created_at
    );
};

template<>
struct glz::meta<DnsPodRecord> {
    using T = DnsPodRecord;
    static constexpr auto value = object(
        "id", &T::id,
        "name", &T::name,
        "value", &T::value
    );
};

#endif //YADDNSC_DRV_DNSPOD_RESPONSE_H
