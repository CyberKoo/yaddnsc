//
// Created by Kotarou on 2022/4/9.
//

#ifndef YADDNSC_BAD_DRIVER_EXCEPTION_H
#define YADDNSC_BAD_DRIVER_EXCEPTION_H

#include "base_exception.h"

class BadDriverException : public YaddnscException {
public:
    using YaddnscException::YaddnscException;

    [[nodiscard]] std::string_view get_name() const override {
        return "BadDriverException";
    }
};

#endif //YADDNSC_BAD_DRIVER_EXCEPTION_H
