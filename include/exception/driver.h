//
// Created by Kotarou on 2022/4/7.
//

#ifndef YADDNSC_EXCEPTION_DRIVER_H
#define YADDNSC_EXCEPTION_DRIVER_H

#include "base.h"

class DriverException : public YaddnscException {
public:
    using YaddnscException::YaddnscException;
};

#endif //YADDNSC_EXCEPTION_DRIVER_H
