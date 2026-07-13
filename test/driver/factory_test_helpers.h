//
// Shared test helper: declares the C entry points produced by DEFINE_DRIVER_FACTORY.
//
// Each driver test .cpp compiles its own driver .cpp (hence one set of factory
// symbols).  Including this header lets tests call create()/destroy()/etc.
// to exercise these paths and improve branch coverage.
// =============================================================================

#ifndef YADDNSC_DRIVER_FACTORY_TEST_HELPERS_H
#define YADDNSC_DRIVER_FACTORY_TEST_HELPERS_H

#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>

#include "interface/driver.h"

// ── C entry points defined by DEFINE_DRIVER_FACTORY ─────────────────────────
extern "C" Driver* create();
extern "C" void destroy(Driver* p);
extern "C" std::uint64_t yaddnsc_drv_magic();
extern "C" std::uint64_t yaddnsc_drv_compiler_id_hash();
extern "C" const char* yaddnsc_drv_build_id_str();

// ── Reusable test cases (use via TYPED_TEST or include in each test suite) ──
// To avoid repeating these in every test file, they are provided as inline
// helper functions.

inline void test_factory_create_destroy() {
    Driver* d = create();
    ASSERT_NE(d, nullptr);
    destroy(d);
}

inline void test_factory_magic() {
    EXPECT_NE(yaddnsc_drv_magic(), 0ULL);
}

inline void test_factory_build_id() {
    const char* id = yaddnsc_drv_build_id_str();
    ASSERT_NE(id, nullptr);
    EXPECT_GT(std::strlen(id), 0u);
}

inline void test_factory_compiler_id_hash() {
    EXPECT_NE(yaddnsc_drv_compiler_id_hash(), 0ULL);
}

#endif // YADDNSC_DRIVER_FACTORY_TEST_HELPERS_H
