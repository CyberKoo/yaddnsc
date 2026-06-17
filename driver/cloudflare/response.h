//
// Created by Kotarou on 2026/6/17.
//
#ifndef YADDNSC_DIGITAL_OCEAN_RESPONSE_H
#define YADDNSC_DIGITAL_OCEAN_RESPONSE_H

#include <cstdint>
#include <glaze/glaze.hpp>

struct CloudflareError {
    int64_t code = 0;
    std::string message = "unknown";
};

struct CloudflareResponse {
    bool success = false;
    std::vector<CloudflareError> errors;
};

template <>
struct glz::meta<CloudflareError> {
    using T = CloudflareError;
    static constexpr auto value = object(
        "code", &T::code,
        "message", &T::message
    );
};

template <>
struct glz::meta<CloudflareResponse> {
    using T = CloudflareResponse;
    static constexpr auto value = object(
        "success", &T::success,
        "errors", &T::errors
    );
};

#endif // YADDNSC_DIGITAL_OCEAN_RESPONSE_H