//
// Created by Kotarou on 2022/4/9.
//

#ifndef YADDNSC_EXCEPTION_BAD_DRIVER_H
#define YADDNSC_EXCEPTION_BAD_DRIVER_H

#include "base.h"

class BadDriverException : public YaddnscException {
public:
    using YaddnscException::YaddnscException;

    [[nodiscard]] std::string_view get_name() const override {
        return "BadDriverException";
    }
};

#endif //YADDNSC_EXCEPTION_BAD_DRIVER_H
