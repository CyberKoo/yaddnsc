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

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#include <string_view>

#include <gtest/gtest.h>

#include "domain/config/runtime_config.h"
#include "core/driver_loader.h"
#include "core/driver_manager.h"
#include "exception/bad_driver.h"
#include "exception/config_verification.h"
#include "exception/driver_not_found.h"

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
//  DriverLoader + DriverManager  (integration via real .so)
// ===========================================================================

TEST(DriverLoaderTest, LoadSimpleDriver_ByName) {
    DriverManager mgr;

    // Build settings: load "simple.so" from test driver dir.
    auto settings = make_load_settings("simple/simple.so");

    // This should dlopen simple.so, verify magic+compiler+ABI, and register.
    EXPECT_NO_THROW({ DriverLoader::load(mgr, settings); });

    // Verify the driver was registered.
    auto loaded = mgr.get_loaded_drivers();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0], "simple");

    // Verify we can retrieve it.
    EXPECT_NO_THROW({ [[maybe_unused]] auto &drv = mgr.get_driver("simple"); });
}

TEST(DriverLoaderTest, LoadDriver_NotFound_Throws) {
    DriverManager mgr;
    domain::DriverSettings settings;
    settings.auto_discover = false;
    settings.load.push_back("nonexistent_driver.so");

    // The file doesn't exist — should throw BadDriverException.
    EXPECT_THROW({ DriverLoader::load(mgr, settings); }, BadDriverException);
}

TEST(DriverLoaderTest, EmptyDriverDir_Throws) {
    DriverManager mgr;
    domain::DriverSettings settings;
    settings.auto_discover = false;
    // driver_dir is set but empty — a configuration error, not a driver error.
    settings.driver_dir = "";
    settings.load.push_back("simple/simple.so");

    EXPECT_THROW({ DriverLoader::load(mgr, settings); }, ConfigVerificationException);
}

TEST(DriverManagerTest, UnloadDriver) {
    DriverManager mgr;
    auto settings = make_load_settings("simple/simple.so");
    DriverLoader::load(mgr, settings);

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
    auto settings = make_load_settings("simple/simple.so");
    DriverLoader::load(mgr, settings);

    // Load again — should log a warning but not throw.
    EXPECT_NO_THROW({ DriverLoader::load(mgr, settings); });

    auto loaded = mgr.get_loaded_drivers();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0], "simple");
}

TEST(DriverLoaderTest, AutoDiscover_NonExistentDir_DoesNotThrow) {
    DriverManager mgr;
    domain::DriverSettings settings;
    settings.auto_discover = true;
    settings.driver_dir = "/nonexistent_directory_for_testing";

    // Should log a warning but not throw.
    EXPECT_NO_THROW({ DriverLoader::load(mgr, settings); });
    EXPECT_TRUE(mgr.get_loaded_drivers().empty());
}

TEST(DriverLoaderTest, AutoDiscover_FileInsteadOfDirectory_DoesNotThrow) {
    char path_template[] = "/tmp/yaddnsc_driver_file_XXXXXX";
    const auto fd = ::mkstemp(path_template);
    ASSERT_GE(fd, 0) << "mkstemp failed";
    ::close(fd);

    DriverManager mgr;
    domain::DriverSettings settings;
    settings.auto_discover = true;
    settings.driver_dir = path_template;
    EXPECT_NO_THROW({ DriverLoader::load(mgr, settings); });
    EXPECT_TRUE(mgr.get_loaded_drivers().empty());

    ::unlink(path_template);
}

TEST(DriverLoaderTest, LoadByAbsolutePath) {
    DriverManager mgr;
    domain::DriverSettings settings;
    settings.auto_discover = false;
    // Use absolute path.
    settings.load.push_back(std::string(TEST_DRIVER_DIR) + "/simple/simple.so");

    EXPECT_NO_THROW({ DriverLoader::load(mgr, settings); });
    ASSERT_EQ(mgr.get_loaded_drivers().size(), 1u);
    EXPECT_EQ(mgr.get_loaded_drivers()[0], "simple");
}

TEST(DriverLoaderTest, DriverCreateReturnsNull_Rejected) {
    DriverManager mgr;
    domain::DriverSettings settings;
    settings.auto_discover = false;
    // The fixture passes magic + compiler verification, but its create()
    // returns nullptr — must be rejected as a bad driver rather than
    // dereferenced downstream.
    settings.load.push_back(NULL_DRIVER_FIXTURE);

    try {
        DriverLoader::load(mgr, settings);
        FAIL() << "Expected BadDriverException";
    } catch (const BadDriverException &e) {
        // Assert the rejection comes from the null-instance guard, not from
        // an earlier magic/compiler mismatch.
        EXPECT_NE(std::string_view(e.what()).find("null instance"), std::string_view::npos);
    }
    EXPECT_TRUE(mgr.get_loaded_drivers().empty());
}

// ===========================================================================
// Auto-discovery resilience (foreign/broken libraries are skipped)
// ===========================================================================

TEST(DriverLoaderTest, AutoDiscover_SkipsBadLibraries) {
    // Temp dir containing one good driver and two bad libraries:
    //  - a text file named .so            (dlopen fails)
    //  - a real shared library that is NOT a yaddnsc driver (dlopen
    //    succeeds, magic/ABI verification fails)
    char dir_template[] = "/tmp/yaddnsc_driver_test_XXXXXX";
    auto *dir = ::mkdtemp(dir_template);
    ASSERT_NE(dir, nullptr) << "mkdtemp failed";

    std::filesystem::copy_file(std::string(TEST_DRIVER_DIR) + "/simple/simple.so",
                               std::string(dir) + "/simple.so");
    {
        FILE *f = std::fopen((std::string(dir) + "/not_elf.so").c_str(), "w");
        ASSERT_NE(f, nullptr);
        std::fputs("this is not a shared library", f);
        std::fclose(f);
    }
    std::filesystem::copy_file(BAD_DRIVER_FIXTURE, std::string(dir) + "/foreign.so");

    DriverManager mgr;
    domain::DriverSettings settings;
    settings.auto_discover = true;
    settings.driver_dir = dir;

    // Before the fix, the foreign libraries aborted the whole load.
    EXPECT_NO_THROW({ DriverLoader::load(mgr, settings); });

    auto loaded = mgr.get_loaded_drivers();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0], "simple");

    std::filesystem::remove_all(dir);
}

TEST(DriverLoaderTest, AutoDiscover_IgnoresNonSharedFiles) {
    char dir_template[] = "/tmp/yaddnsc_driver_test_XXXXXX";
    auto *dir = ::mkdtemp(dir_template);
    ASSERT_NE(dir, nullptr) << "mkdtemp failed";

    std::filesystem::copy_file(std::string(TEST_DRIVER_DIR) + "/simple/simple.so",
                               std::string(dir) + "/simple.so");
    {
        FILE *f = std::fopen((std::string(dir) + "/README.txt").c_str(), "w");
        ASSERT_NE(f, nullptr);
        std::fputs("not a driver", f);
        std::fclose(f);
    }
    std::filesystem::create_directory(std::string(dir) + "/nested.so");

    DriverManager mgr;
    domain::DriverSettings settings;
    settings.auto_discover = true;
    settings.driver_dir = dir;
    EXPECT_NO_THROW({ DriverLoader::load(mgr, settings); });
    ASSERT_EQ(mgr.get_loaded_drivers().size(), 1u);
    EXPECT_EQ(mgr.get_loaded_drivers()[0], "simple");

    std::filesystem::remove_all(dir);
}

TEST(DriverLoaderTest, AutoDiscover_AllBad_DoesNotThrow) {
    char dir_template[] = "/tmp/yaddnsc_driver_test_XXXXXX";
    auto *dir = ::mkdtemp(dir_template);
    ASSERT_NE(dir, nullptr) << "mkdtemp failed";

    std::filesystem::copy_file(BAD_DRIVER_FIXTURE, std::string(dir) + "/foreign.so");

    DriverManager mgr;
    domain::DriverSettings settings;
    settings.auto_discover = true;
    settings.driver_dir = dir;

    EXPECT_NO_THROW({ DriverLoader::load(mgr, settings); });
    EXPECT_TRUE(mgr.get_loaded_drivers().empty());

    std::filesystem::remove_all(dir);
}
