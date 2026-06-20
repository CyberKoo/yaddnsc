//
// Created by Kotarou on 2022/4/7.
//

#ifndef YADDNSC_DRIVER_MISSING_REQUIRED_PARAM_EXCEPTION_H
#define YADDNSC_DRIVER_MISSING_REQUIRED_PARAM_EXCEPTION_H

#include <exception/driver_exception.h>

class ParamParseException : public DriverException {
public:
    using DriverException::DriverException;

    [[nodiscard]] std::string_view get_name() const override {
        return "ParamParseException";
    }
};

#endif //YADDNSC_DRIVER_MISSING_REQUIRED_PARAM_EXCEPTION_H
