//
// Unit tests for src/infrastructure/plugin/driver_loader.cpp +
// src/infrastructure/plugin/driver_catalog.cpp
//
// Loads a real .so driver (simple.so) from the build tree, exercises
// DriverLoader::load(), DriverCatalog::find()/get_descriptor(),
// get_loaded_drivers(), and unload_driver().
//
// NOTE: The TEST_DRIVER_DIR compile definition must point to the directory
// containing built .so driver files (typically ${CMAKE_BINARY_DIR}/driver).
// =============================================================================

#include "infrastructure/plugin/driver_loader.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>

#include "domain/config/runtime_config.h"
#include "infrastructure/config/config_verification_exception.h"
#include "infrastructure/plugin/driver_catalog.h"
#include "infrastructure/plugin/driver_not_found_exception.h"
#include "infrastructure/plugin/plugin_load_exception.h"

// ── Helpers ─────────────────────────────────────────────────────────────────

/// Build driver settings that load a single driver by name from the test dir.
[[nodiscard]] domain::DriverSettings make_load_settings(std::string_view driver_name) {
    domain::DriverSettings settings;
    settings.auto_discover = false;
    settings.driver_dir = std::filesystem::path(TEST_DRIVER_DIR);
    settings.load.push_back(std::string(driver_name));
    return settings;
}

// ===========================================================================
//  DriverLoader + DriverCatalog  (integration via real .so)
// ===========================================================================

TEST(DriverLoaderTest, LoadSimpleDriver_ByName) {
    DriverCatalog catalog;

    // Build settings: load "simple.so" from test driver dir.
    auto settings = make_load_settings("simple/simple.so");

    // This should dlopen simple.so, verify magic+revision, and register.
    EXPECT_NO_THROW({ DriverLoader::load(catalog, settings); });

    // Verify the driver was registered.
    auto loaded = catalog.get_loaded_drivers();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0], "simple");

    // Verify we can retrieve it.
    EXPECT_NO_THROW({ [[maybe_unused]] auto& module = catalog.get_descriptor("simple"); });
    EXPECT_NE(catalog.find("simple"), nullptr);
}

TEST(DriverLoaderTest, LoadDriver_NotFound_Throws) {
    DriverCatalog catalog;
    domain::DriverSettings settings;
    settings.auto_discover = false;
    settings.load.push_back("nonexistent_driver.so");

    // The file doesn't exist — should throw PluginLoadException.
    EXPECT_THROW({ DriverLoader::load(catalog, settings); }, PluginLoadException);
}

TEST(DriverLoaderTest, EmptyDriverDir_Throws) {
    DriverCatalog catalog;
    domain::DriverSettings settings;
    settings.auto_discover = false;
    // driver_dir is set but empty — a configuration error, not a driver error.
    settings.driver_dir = "";
    settings.load.push_back("simple/simple.so");

    EXPECT_THROW({ DriverLoader::load(catalog, settings); }, ConfigVerificationException);
}

TEST(DriverCatalogTest, UnloadDriver) {
    DriverCatalog catalog;
    auto settings = make_load_settings("simple/simple.so");
    DriverLoader::load(catalog, settings);

    // Verify it's loaded.
    ASSERT_EQ(catalog.get_loaded_drivers().size(), 1u);

    // Unload it.
    EXPECT_NO_THROW({ catalog.unload_driver("simple"); });
    EXPECT_TRUE(catalog.get_loaded_drivers().empty());
}

TEST(DriverCatalogTest, UnloadDriver_NotFound_Throws) {
    DriverCatalog catalog;
    EXPECT_THROW({ catalog.unload_driver("nonexistent"); }, DriverNotFoundException);
}

TEST(DriverCatalogTest, GetDescriptor_NotFound_Throws) {
    DriverCatalog catalog;
    EXPECT_THROW({ [[maybe_unused]] auto& d = catalog.get_descriptor("nonexistent"); }, DriverNotFoundException);
}

TEST(DriverCatalogTest, LoadSameDriverTwice_SecondIsSkipped) {
    DriverCatalog catalog;
    auto settings = make_load_settings("simple/simple.so");
    DriverLoader::load(catalog, settings);

    // Load again — should log a warning but not throw.
    EXPECT_NO_THROW({ DriverLoader::load(catalog, settings); });

    auto loaded = catalog.get_loaded_drivers();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0], "simple");
}

TEST(DriverLoaderTest, AutoDiscover_NonExistentDir_DoesNotThrow) {
    DriverCatalog catalog;
    domain::DriverSettings settings;
    settings.auto_discover = true;
    settings.driver_dir = "/nonexistent_directory_for_testing";

    // Should log a warning but not throw.
    EXPECT_NO_THROW({ DriverLoader::load(catalog, settings); });
    EXPECT_TRUE(catalog.get_loaded_drivers().empty());
}

TEST(DriverLoaderTest, AutoDiscover_FileInsteadOfDirectory_DoesNotThrow) {
    char path_template[] = "/tmp/yaddnsc_driver_file_XXXXXX";
    const auto fd = ::mkstemp(path_template);
    ASSERT_GE(fd, 0) << "mkstemp failed";
    ::close(fd);

    DriverCatalog catalog;
    domain::DriverSettings settings;
    settings.auto_discover = true;
    settings.driver_dir = path_template;
    EXPECT_NO_THROW({ DriverLoader::load(catalog, settings); });
    EXPECT_TRUE(catalog.get_loaded_drivers().empty());

    ::unlink(path_template);
}

TEST(DriverLoaderTest, LoadByAbsolutePath) {
    DriverCatalog catalog;
    domain::DriverSettings settings;
    settings.auto_discover = false;
    // Use absolute path.
    settings.load.push_back(std::string(TEST_DRIVER_DIR) + "/simple/simple.so");

    EXPECT_NO_THROW({ DriverLoader::load(catalog, settings); });
    ASSERT_EQ(catalog.get_loaded_drivers().size(), 1u);
    EXPECT_EQ(catalog.get_loaded_drivers()[0], "simple");
}

// ===========================================================================
// Auto-discovery resilience (foreign/broken libraries are skipped)
// ===========================================================================

TEST(DriverLoaderTest, AutoDiscover_SkipsBadLibraries) {
    // Temp dir containing one good driver and two bad libraries:
    //  - a text file named .so            (dlopen fails)
    //  - a real shared library that is NOT a yaddnsc driver (dlopen
    //    succeeds, entry-point resolution fails)
    char dir_template[] = "/tmp/yaddnsc_driver_test_XXXXXX";
    auto* dir = ::mkdtemp(dir_template);
    ASSERT_NE(dir, nullptr) << "mkdtemp failed";

    std::filesystem::copy_file(std::string(TEST_DRIVER_DIR) + "/simple/simple.so", std::string(dir) + "/simple.so");
    {
        FILE* f = std::fopen((std::string(dir) + "/not_elf.so").c_str(), "w");
        ASSERT_NE(f, nullptr);
        std::fputs("this is not a shared library", f);
        std::fclose(f);
    }
    std::filesystem::copy_file(BAD_DRIVER_FIXTURE, std::string(dir) + "/foreign.so");

    DriverCatalog catalog;
    domain::DriverSettings settings;
    settings.auto_discover = true;
    settings.driver_dir = dir;

    // Foreign libraries must be skipped, not abort the whole load.
    EXPECT_NO_THROW({ DriverLoader::load(catalog, settings); });

    auto loaded = catalog.get_loaded_drivers();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0], "simple");

    std::filesystem::remove_all(dir);
}

TEST(DriverLoaderTest, AutoDiscover_IgnoresNonSharedFiles) {
    char dir_template[] = "/tmp/yaddnsc_driver_test_XXXXXX";
    auto* dir = ::mkdtemp(dir_template);
    ASSERT_NE(dir, nullptr) << "mkdtemp failed";

    std::filesystem::copy_file(std::string(TEST_DRIVER_DIR) + "/simple/simple.so", std::string(dir) + "/simple.so");
    {
        FILE* f = std::fopen((std::string(dir) + "/README.txt").c_str(), "w");
        ASSERT_NE(f, nullptr);
        std::fputs("not a driver", f);
        std::fclose(f);
    }
    std::filesystem::create_directory(std::string(dir) + "/nested.so");

    DriverCatalog catalog;
    domain::DriverSettings settings;
    settings.auto_discover = true;
    settings.driver_dir = dir;
    EXPECT_NO_THROW({ DriverLoader::load(catalog, settings); });
    ASSERT_EQ(catalog.get_loaded_drivers().size(), 1u);
    EXPECT_EQ(catalog.get_loaded_drivers()[0], "simple");

    std::filesystem::remove_all(dir);
}

TEST(DriverLoaderTest, AutoDiscover_AllBad_DoesNotThrow) {
    char dir_template[] = "/tmp/yaddnsc_driver_test_XXXXXX";
    auto* dir = ::mkdtemp(dir_template);
    ASSERT_NE(dir, nullptr) << "mkdtemp failed";

    std::filesystem::copy_file(BAD_DRIVER_FIXTURE, std::string(dir) + "/foreign.so");

    DriverCatalog catalog;
    domain::DriverSettings settings;
    settings.auto_discover = true;
    settings.driver_dir = dir;

    EXPECT_NO_THROW({ DriverLoader::load(catalog, settings); });
    EXPECT_TRUE(catalog.get_loaded_drivers().empty());

    std::filesystem::remove_all(dir);
}
