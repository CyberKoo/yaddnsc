//
// Created by kotarou on 2026/6/17.
//

#ifndef YADDNSC_DRIVER_DRIVER_FACTORY_H
#define YADDNSC_DRIVER_DRIVER_FACTORY_H

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

#define DEFINE_DRIVER_FACTORY(DriverClass)     \
extern "C" EXPORT IDriver* create() {          \
    return new DriverClass();                  \
}                                              \
                                               \
extern "C" EXPORT void destroy(IDriver* p) {   \
    delete p;                                  \
}
#endif //YADDNSC_DRIVER_DRIVER_FACTORY_H
