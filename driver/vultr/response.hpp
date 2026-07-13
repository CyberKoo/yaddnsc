//
// Created by Kotarou on 2026/7/13.
//

#ifndef YADDNSC_DRV_VULTR_RESPONSE_H
#define YADDNSC_DRV_VULTR_RESPONSE_H

#include <string>
#include <vector>
#include <optional>
#include <glaze/glaze.hpp>

/// Vultr API error detail.
struct VultrError {
    std::string detail;  ///< Error description
};

/// Vultr API error response body.
struct VultrErrorResponse {
    std::vector<VultrError> errors;  ///< List of errors
};

template<>
struct glz::meta<VultrError> {
    using T = VultrError;
    static constexpr auto value = object(
        "detail", &T::detail
    );
};

template<>
struct glz::meta<VultrErrorResponse> {
    using T = VultrErrorResponse;
    static constexpr auto value = object(
        "errors", &T::errors
    );
};

#endif // YADDNSC_DRV_VULTR_RESPONSE_H
