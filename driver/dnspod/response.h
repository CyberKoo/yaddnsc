//
// Created by kotarou on 2026/6/17.
//

#ifndef YADDNSC_DRV_DNSPOD_RESPONSE_H
#define YADDNSC_DRV_DNSPOD_RESPONSE_H

#include <string>
#include <optional>

#include <glaze/glaze.hpp>

struct DnsPodStatus {
    std::string code;
    std::string message;
};

struct DnsPodResponse {
    std::optional<DnsPodStatus> status;
};

template<>
struct glz::meta<DnsPodStatus> {
    using T = DnsPodStatus;
    static constexpr auto value = object(
        "code", &T::code,
        "message", &T::message
    );
};

#endif //YADDNSC_DRV_DNSPOD_RESPONSE_H
