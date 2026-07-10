//
// Created by Kotarou on 2026/7/11.
//

#ifndef YADDNSC_EXCEPTION_DRIVER_NOT_FOUND_H
#define YADDNSC_EXCEPTION_DRIVER_NOT_FOUND_H

#include "base.h"

/// Thrown when a lookup is performed for a driver name that has not been
/// registered with the DriverManager.
class DriverNotFoundException : public YaddnscException {
public:
    using YaddnscException::YaddnscException;

    [[nodiscard]] std::string_view get_name() const noexcept override {
        return "DriverNotFoundException";
    }
};

#endif // YADDNSC_EXCEPTION_DRIVER_NOT_FOUND_H
