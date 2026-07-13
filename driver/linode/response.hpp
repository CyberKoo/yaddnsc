//
// Created by Kotarou on 2026/7/13.
//

#ifndef YADDNSC_DRV_LINODE_RESPONSE_H
#define YADDNSC_DRV_LINODE_RESPONSE_H

#include <string>
#include <optional>
#include <vector>
#include <glaze/glaze.hpp>

/// Linode API error detail.
struct LinodeError {
    std::string field;   ///< Field that caused the error (may be null)
    std::string reason;  ///< Error description
};

/// Linode API error response body.
struct LinodeErrorResponse {
    std::vector<LinodeError> errors;  ///< List of errors
};

template<>
struct glz::meta<LinodeError> {
    using T = LinodeError;
    static constexpr auto value = object(
        "field", &T::field,
        "reason", &T::reason
    );
};

template<>
struct glz::meta<LinodeErrorResponse> {
    using T = LinodeErrorResponse;
    static constexpr auto value = object(
        "errors", &T::errors
    );
};

#endif // YADDNSC_DRV_LINODE_RESPONSE_H
