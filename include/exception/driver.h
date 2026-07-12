//
// Created by Kotarou on 2022/4/7.
//

#ifndef YADDNSC_EXCEPTION_DRIVER_H
#define YADDNSC_EXCEPTION_DRIVER_H

#include "base.h"

/// Base exception class for all driver-related errors.
///
/// Specialised subclasses (e.g. ParamParseException) provide more granular
/// error types.
///
/// @note  This class remains abstract (pure virtual get_name() inherited from
///        YaddnscException).  Every concrete driver exception must override
///        get_name().  Do not instantiate DriverException directly.
class DriverException : public YaddnscException {
public:
    using YaddnscException::YaddnscException;
};

#endif //YADDNSC_EXCEPTION_DRIVER_H
