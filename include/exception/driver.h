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
class DriverException : public YaddnscException {
public:
    using YaddnscException::YaddnscException;
};

#endif //YADDNSC_EXCEPTION_DRIVER_H
