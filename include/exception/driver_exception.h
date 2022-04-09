//
// Created by Kotarou on 2022/4/7.
//

#ifndef YADDNSC_DRIVER_EXCEPTION_H
#define YADDNSC_DRIVER_EXCEPTION_H

#include "base_exception.h"

class DriverException : public YaddnscException {
public:
    using YaddnscException::YaddnscException;

    [[nodiscard]] std::string_view get_name() const override {
        return "DriverException";
    }
};

#endif //YADDNSC_DRIVER_EXCEPTION_H
