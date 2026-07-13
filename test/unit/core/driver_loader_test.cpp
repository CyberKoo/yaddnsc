//
// Unit tests for src/core/driver_loader.cpp + src/core/driver_manager.cpp
//
// Loads a real .so driver (simple.so) from the build tree, exercises
// DriverLoader::load(), DriverManager::get_driver(), get_loaded_drivers(),
// and unload_driver().
//
// NOTE: The TEST_DRIVER_DIR compile definition must point to the directory
// containing built .so driver files (typically ${CMAKE_BINARY_DIR}/driver).
// =============================================================================

#include <string>
#include <vector>
#include <string_view>

#include <gtest/gtest.h>

#include "config/config.h"
#include "core/driver_loader.h"
#include "core/driver_manager.h"
#include "exception/bad_driver.h"
#include "exception/driver_not_found.h"

// ── Helpers ─────────────────────────────────────────────────────────────────

/// Build an AppConfig that loads a single driver by name from the test dir.
[[nodiscard]] Config::AppConfig make_load_config(std::string_view driver_name) {
    Config::AppConfig cfg;
    cfg.driver.auto_discover = false;
    cfg.driver.driver_dir = TEST_DRIVER_DIR;
    cfg.driver.load.push_back(std::string(driver_name));
    return cfg;
}

[[nodiscard]] Config::AppConfig make_auto_discover_config() {
    Config::AppConfig cfg;
    cfg.driver.auto_discover = true;
    cfg.driver.driver_dir = TEST_DRIVER_DIR;
    return cfg;
}

// ===========================================================================
//  DriverLoader + DriverManager  (integration via real .so)
// ===========================================================================

TEST(DriverLoaderTest, LoadSimpleDriver_ByName) {
    DriverManager mgr;

    // Build config: load "simple.so" from test driver dir.
    auto cfg = make_load_config("simple/simple.so");

    // This should dlopen simple.so, verify magic+compiler+ABI, and register.
    EXPECT_NO_THROW({ DriverLoader::load(mgr, cfg); });

    // Verify the driver was registered.
    auto loaded = mgr.get_loaded_drivers();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0], "simple");

    // Verify we can retrieve it.
    EXPECT_NO_THROW({ [[maybe_unused]] auto &drv = mgr.get_driver("simple"); });
}

TEST(DriverLoaderTest, LoadDriver_NotFound_Throws) {
    DriverManager mgr;
    Config::AppConfig cfg;
    cfg.driver.auto_discover = false;
    cfg.driver.load.push_back("nonexistent_driver.so");

    // The file doesn't exist — should throw BadDriverException.
    EXPECT_THROW({ DriverLoader::load(mgr, cfg); }, BadDriverException);
}

TEST(DriverManagerTest, UnloadDriver) {
    DriverManager mgr;
    auto cfg = make_load_config("simple/simple.so");
    DriverLoader::load(mgr, cfg);

    // Verify it's loaded.
    ASSERT_EQ(mgr.get_loaded_drivers().size(), 1u);

    // Unload it.
    EXPECT_NO_THROW({ mgr.unload_driver("simple"); });
    EXPECT_TRUE(mgr.get_loaded_drivers().empty());
}

TEST(DriverManagerTest, UnloadDriver_NotFound_Throws) {
    DriverManager mgr;
    EXPECT_THROW({ mgr.unload_driver("nonexistent"); }, DriverNotFoundException);
}

TEST(DriverManagerTest, GetDriver_NotFound_Throws) {
    DriverManager mgr;
    EXPECT_THROW({ [[maybe_unused]] auto &d = mgr.get_driver("nonexistent"); },
                 DriverNotFoundException);
}

TEST(DriverManagerTest, LoadSameDriverTwice_SecondIsSkipped) {
    DriverManager mgr;
    auto cfg = make_load_config("simple/simple.so");
    DriverLoader::load(mgr, cfg);

    // Load again — should log a warning but not throw.
    EXPECT_NO_THROW({ DriverLoader::load(mgr, cfg); });

    auto loaded = mgr.get_loaded_drivers();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0], "simple");
}

TEST(DriverLoaderTest, AutoDiscover_NonExistentDir_DoesNotThrow) {
    DriverManager mgr;
    Config::AppConfig cfg;
    cfg.driver.auto_discover = true;
    cfg.driver.driver_dir = "/nonexistent_directory_for_testing";

    // Should log a warning but not throw.
    EXPECT_NO_THROW({ DriverLoader::load(mgr, cfg); });
    EXPECT_TRUE(mgr.get_loaded_drivers().empty());
}

TEST(DriverLoaderTest, LoadByAbsolutePath) {
    DriverManager mgr;
    Config::AppConfig cfg;
    cfg.driver.auto_discover = false;
    // Use absolute path.
    cfg.driver.load.push_back(std::string(TEST_DRIVER_DIR) + "/simple/simple.so");

    EXPECT_NO_THROW({ DriverLoader::load(mgr, cfg); });
    ASSERT_EQ(mgr.get_loaded_drivers().size(), 1u);
    EXPECT_EQ(mgr.get_loaded_drivers()[0], "simple");
}
