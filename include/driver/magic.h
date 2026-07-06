//
// Created by Dafan Wang on 4/7/26.
//

#ifndef YADDNSC_DRIVER_MAGIC_H
#define YADDNSC_DRIVER_MAGIC_H

#include <cstdint>

/// Magic constant used to verify that a shared library is indeed a yaddnsc driver.
///
/// The host checks this via `yaddnsc_drv_magic()` before calling `create()`
/// to reject unrelated .so files.
constexpr std::uint64_t YADDNSC_DRIVER_MAGIC = 0x5941444E53430000ULL; // "YADDNSC\0\0"

#endif //YADDNSC_DRIVER_MAGIC_H
