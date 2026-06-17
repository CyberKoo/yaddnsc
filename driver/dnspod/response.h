//
// Created by kotarou on 2026/6/17.
//

#ifndef YADDNSC_DRV_DNSPOD_RESPONSE_H
#define YADDNSC_DRV_DNSPOD_RESPONSE_H

#include <cstdint>
#include <string>
#include <optional>

#include <glaze/glaze.hpp>

struct DnsPodStatus {
    std::string code;
    std::string message;
    std::string created_at;
};

struct DnsPodRecord {
    int64_t id{};
    std::string name;
    std::string value;
};

struct DnsPodResponse {
    std::optional<DnsPodStatus> status;
    std::optional<DnsPodRecord> record;
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
