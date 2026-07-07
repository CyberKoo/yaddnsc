//
// Created by Kotarou on 2022/4/7.
//

#ifndef YADDNSC_DRIVER_MISSING_REQUIRED_PARAM_EXCEPTION_H
#define YADDNSC_DRIVER_MISSING_REQUIRED_PARAM_EXCEPTION_H

#include "exception/driver.h"

/// Thrown when a driver's JSON configuration cannot be parsed or is missing
/// required fields.
///
/// The message contains the glaze-formatted error describing which field
/// is missing or malformed.
class ParamParseException : public DriverException {
public:
    using DriverException::DriverException;

    [[nodiscard]] std::string_view get_name() const noexcept override {
        return "ParamParseException";
    }
};

#endif //YADDNSC_DRIVER_MISSING_REQUIRED_PARAM_EXCEPTION_H
