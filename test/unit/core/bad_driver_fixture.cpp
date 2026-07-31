//
// Test fixture: a shared library that is deliberately NOT a yaddnsc driver.
//
// Used by driver_loader_test.cpp to verify that driver auto-discovery skips
// foreign/broken libraries (dlopen may succeed, but the yaddnsc_drv_* magic
// and ABI checks fail) instead of aborting startup.
//
// No yaddnsc driver entry points are exported.
// =============================================================================

// A non-static symbol so the linker keeps the library non-empty.
int yaddnsc_bad_driver_fixture_value = 42;
