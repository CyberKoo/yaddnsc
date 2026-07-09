//
// Created by kotarou on 2026/6/17.
//

#ifndef YADDNSC_DRIVER_FACTORY_H
#define YADDNSC_DRIVER_FACTORY_H

#include "build_id.hpp"  // IWYU pragma: keep — used by DEFINE_DRIVER_FACTORY macro at expansion time
#include "magic.h"  // IWYU pragma: keep — used by DEFINE_DRIVER_FACTORY macro at expansion time
#include "yaddnsc_export.h"

/// Define the five C entry points required by every yaddnsc driver shared
/// library.
///
/// Expands to:
///   - `create()`    — constructs a new DriverClass instance
///   - `destroy(Driver*)` — deletes a DriverClass instance
///   - `yaddnsc_drv_magic()` — returns the driver magic constant
///   - `yaddnsc_drv_compiler_id_hash()` — FNV-1a hash of compiler identity fields
///   - `yaddnsc_drv_build_id_str()` — human-readable build ID string
///
/// Usage in a driver plugin:
/// @code{.cpp}
/// class MyDriver : public BaseDriver { ... };
/// DEFINE_DRIVER_FACTORY(MyDriver)
/// @endcode
#define DEFINE_DRIVER_FACTORY(DriverClass)                    \
extern "C" YADDNSC_EXPORT Driver* create() {                  \
    return new DriverClass(); /* NOLINT(cppcoreguidelines-owning-memory) */ \
}                                                             \
                                                              \
extern "C" YADDNSC_EXPORT void destroy(Driver* p) {           \
    delete p; /* NOLINT(cppcoreguidelines-owning-memory) */   \
}                                                             \
                                                              \
extern "C" YADDNSC_EXPORT std::uint64_t yaddnsc_drv_magic() { \
    return YADDNSC_DRIVER_MAGIC;                              \
}                                                             \
                                                              \
extern "C" YADDNSC_EXPORT std::uint64_t yaddnsc_drv_compiler_id_hash() { \
    return BuildId::COMPILER_ID_HASH;                           \
}                                                             \
                                                              \
extern "C" YADDNSC_EXPORT const char* yaddnsc_drv_build_id_str() { \
    return BuildId::full_id();                                   \
}

#endif //YADDNSC_DRIVER_FACTORY_H
