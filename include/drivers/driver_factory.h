//
// Created by kotarou on 2026/6/17.
//

#ifndef YADDNSC_DRIVER_DRIVER_FACTORY_H
#define YADDNSC_DRIVER_DRIVER_FACTORY_H

#include "yaddnsc_export.h"

#define DEFINE_DRIVER_FACTORY(DriverClass)            \
extern "C" YADDNSC_EXPORT Driver* create() {         \
    return new DriverClass();                         \
}                                                     \
                                                      \
extern "C" YADDNSC_EXPORT void destroy(Driver* p) {  \
    delete p;                                         \
}
#endif //YADDNSC_DRIVER_DRIVER_FACTORY_H
