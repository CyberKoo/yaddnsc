//
// Created by Kotarou on 2022/4/9.
//

#ifndef YADDNSC_EXCEPTION_BAD_DRIVER_H
#define YADDNSC_EXCEPTION_BAD_DRIVER_H

#include "base.h"

/// Thrown when a loaded shared library is not a valid yaddnsc driver
/// (e.g. magic number mismatch or missing entry points).
class BadDriverException : public YaddnscException {
public:
    using YaddnscException::YaddnscException;

    [[nodiscard]] std::string_view get_name() const noexcept override {
        return "BadDriverException";
    }
};

#endif //YADDNSC_EXCEPTION_BAD_DRIVER_H
