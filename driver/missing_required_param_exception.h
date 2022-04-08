//
// Created by Kotarou on 2022/4/7.
//

#ifndef YADDNSC_MISSING_REQUIRED_PARAM_EXCEPTION_H
#define YADDNSC_MISSING_REQUIRED_PARAM_EXCEPTION_H

#include "exception/driver_exception.h"

class MissingRequiredParamException : public DriverException {
public:
    using DriverException::DriverException;
};

#endif //YADDNSC_MISSING_REQUIRED_PARAM_EXCEPTION_H
