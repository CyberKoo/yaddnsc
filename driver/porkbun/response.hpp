//
// Created by Kotarou on 2026/7/13.
//

#ifndef YADDNSC_DRV_PORKBUN_RESPONSE_H
#define YADDNSC_DRV_PORKBUN_RESPONSE_H

#include <string>
#include <optional>
#include <glaze/glaze.hpp>

/// Porkbun API basic response.
struct PorkbunResponse {
    std::string status;                 ///< "SUCCESS" or "ERROR"
    std::optional<std::string> message; ///< Human-readable message (present on ERROR, sometimes on SUCCESS)
    std::optional<std::string> code;    ///< Machine-readable error code (present when status is ERROR)
};

template<>
struct glz::meta<PorkbunResponse> {
    using T = PorkbunResponse;
    static constexpr auto value = object(
        "status", &T::status,
        "message", &T::message,
        "code", &T::code
    );
};

#endif // YADDNSC_DRV_PORKBUN_RESPONSE_H
