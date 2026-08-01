//
// Test fixture: a shared library that passes the magic and compiler-identity
// checks but whose create() factory returns nullptr.
//
// Used by driver_loader_test.cpp to verify that DriverModule::create_driver
// rejects a null driver instance with BadDriverException instead of
// dereferencing it downstream (DriverManager::register_driver calls
// get_driver() → *driver_ on the loaded instance).
// =============================================================================

#include <cstdint>

#include "build_id.hpp"
#include "driver/magic.h"
#include "yaddnsc_export.h"

class Driver;

extern "C" YADDNSC_EXPORT Driver* create() {
    return nullptr;
}

extern "C" YADDNSC_EXPORT void destroy([[maybe_unused]] Driver* p) {
}

extern "C" YADDNSC_EXPORT std::uint64_t yaddnsc_drv_magic() {
    return YADDNSC_DRIVER_MAGIC;
}

extern "C" YADDNSC_EXPORT std::uint64_t yaddnsc_drv_compiler_id_hash() {
    return BuildId::COMPILER_ID_HASH;
}
