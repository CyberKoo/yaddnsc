//
// Created by Kotarou on 2026/7/13.
//

#ifndef YADDNSC_DRV_VULTR_RESPONSE_H
#define YADDNSC_DRV_VULTR_RESPONSE_H

#include <cstdint>
#include <string>
#include <glaze/glaze.hpp>

/// Vultr API v2 error response body: {"error": "...", "status": 400}
struct VultrErrorResponse {
    std::string error;  ///< Error description
    int64_t status = 0; ///< Status code echoed by the API
};

template<>
struct glz::meta<VultrErrorResponse> {
    using T = VultrErrorResponse;
    static constexpr auto value = object(
        "error", &T::error,
        "status", &T::status
    );
};

#endif // YADDNSC_DRV_VULTR_RESPONSE_H
