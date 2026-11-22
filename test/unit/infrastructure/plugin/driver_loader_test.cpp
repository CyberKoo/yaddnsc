//
// Unit tests for src/infrastructure/plugin/driver_loader.cpp +
// src/infrastructure/plugin/driver_catalog.cpp
//
// Loads a real .so driver (simple.so) from the build tree, exercises
// DriverLoader::load() and the DriverCatalog it feeds: register_package(),
// find(), loaded_drivers(), describe() and unload_driver().
//
// The loader reports instead of throwing: a manual load failure is an
// unexpected DriverLoadError, an auto-discovery skip is a non-fatal entry in
// the report's skipped list, and a missing or unusable driver directory is a
// report warning.
//
// A configured path with an embedded NUL byte is an invalid setting: the
// filesystem and dlopen calls take a C string and would truncate the path,
// so the loader rejects it before it touches the filesystem.
//
// NOTE: The TEST_DRIVER_DIR compile definition must point to the directory
// containing built .so driver files (typically ${CMAKE_BINARY_DIR}/driver).
// =============================================================================

#include "infrastructure/plugin/driver_loader.h"

#include <algorithm>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>

#include "application/ports/driver_catalog.h"
#include "domain/config/runtime_config.h"
#include "domain/error/error.h"
#include "infrastructure/plugin/driver_catalog.h"
#include "infrastructure/plugin/driver_not_found_exception.h"
#include "infrastructure/plugin/plugin_package.h"

// ── Helpers ─────────────────────────────────────────────────────────────────

/// Build driver settings that load a single driver by name from the test dir.
[[nodiscard]] domain::DriverSettings make_load_settings(std::string_view driver_name) {
    domain::DriverSettings settings;
    settings.auto_discover = false;
    settings.driver_dir = std::filesystem::path(TEST_DRIVER_DIR);
    settings.load.emplace_back(driver_name);
    return settings;
}

/// Register every package of a load report into a catalog: the loader owns
/// discovery and validation, the catalog only owns registration.
void register_all(DriverCatalog& catalog, const plugin::DriverLoadReport& report) {
    for (const auto& package : report.packages) {
        catalog.register_package(package);
    }
}

/// A path that carries an embedded NUL byte.
///
/// The C APIs behind the filesystem and dlopen calls stop at the first NUL,
/// so such a path would silently address a different file.
[[nodiscard]] std::string with_embedded_nul(std::string prefix) {
    prefix.push_back('\0');
    prefix += "ignored";
    return prefix;
}

// ===========================================================================
//  DriverLoader  (integration via real .so)
// ===========================================================================

TEST(DriverLoaderTest, LoadSimpleDriver_ByName) {
    // This should dlopen simple.so and verify magic+revision.
    const auto loaded = DriverLoader::load(make_load_settings("simple/simple.so"));
    ASSERT_TRUE(loaded) << loaded.error().message;
    ASSERT_EQ(loaded->packages.size(), 1u);

    DriverCatalog catalog;
    register_all(catalog, *loaded);

    // Verify the driver was registered.
    const auto names = catalog.loaded_drivers();
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "simple");

    // Verify we can retrieve it.
    const auto package = catalog.find("simple");
    ASSERT_NE(package, nullptr);
    EXPECT_EQ(package->descriptor().name, "simple");
}

TEST(DriverLoaderTest, LoadDriver_NotFound_ReportsUnexpected) {
    domain::DriverSettings settings;
    settings.auto_discover = false;
    settings.driver_dir = std::filesystem::path(TEST_DRIVER_DIR);
    settings.load.emplace_back("nonexistent_driver.so");

    // The file doesn't exist: the manual load path fails with an error value.
    const auto loaded = DriverLoader::load(settings);

    ASSERT_FALSE(loaded);
    EXPECT_EQ(loaded.error().code, plugin::DriverLoadError::Code::LOAD_FAILED);
    EXPECT_NE(loaded.error().message.find("nonexistent_driver.so"), std::string::npos) << loaded.error().message;
}

TEST(DriverLoaderTest, EmptyDriverDir_ReportsInvalidSettings) {
    domain::DriverSettings settings;
    settings.auto_discover = false;
    // driver_dir is set but empty — a configuration error, not a driver error.
    settings.driver_dir = std::filesystem::path{};
    settings.load.emplace_back("simple/simple.so");

    // The loader reports the invalid configuration as a value; it does not
    // throw a configuration exception at the caller.
    const auto loaded = DriverLoader::load(settings);

    ASSERT_FALSE(loaded);
    EXPECT_EQ(loaded.error().code, plugin::DriverLoadError::Code::INVALID_SETTINGS);
    EXPECT_NE(loaded.error().message.find("driver_dir"), std::string::npos) << loaded.error().message;
}

TEST(DriverLoaderTest, DriverDirWithEmbeddedNul_IsRejectedBeforeAnyFilesystemAccess) {
    domain::DriverSettings settings;
    settings.auto_discover = false;
    // Truncating the path at the NUL yields the real driver directory: with
    // no rejection the loader would find and load the driver instead of
    // reporting the invalid setting.
    settings.driver_dir = std::filesystem::path(with_embedded_nul(TEST_DRIVER_DIR));
    settings.load.emplace_back("simple/simple.so");

    const auto loaded = DriverLoader::load(settings);

    ASSERT_FALSE(loaded);
    EXPECT_EQ(loaded.error().code, plugin::DriverLoadError::Code::INVALID_SETTINGS);
    EXPECT_EQ(loaded.error().message, "driver_dir contains an embedded NUL byte");
}

TEST(DriverLoaderTest, LoadEntryWithEmbeddedNul_IsRejectedBeforeAnyFilesystemAccess) {
    domain::DriverSettings settings;
    settings.auto_discover = false;
    settings.driver_dir = std::filesystem::path(TEST_DRIVER_DIR);
    // Truncating the path at the NUL yields the real driver file: with no
    // rejection the loader would load that file instead of reporting the
    // invalid setting.
    settings.load.emplace_back(with_embedded_nul("simple/simple.so"));

    const auto loaded = DriverLoader::load(settings);

    ASSERT_FALSE(loaded);
    EXPECT_EQ(loaded.error().code, plugin::DriverLoadError::Code::INVALID_SETTINGS);
    EXPECT_EQ(loaded.error().message, "driver path contains an embedded NUL byte");
}

TEST(DriverLoaderTest, AutoDiscoverWithEmbeddedNulInDriverDir_IsRejectedBeforeEnumeration) {
    domain::DriverSettings settings;
    settings.auto_discover = true;
    settings.driver_dir = std::filesystem::path(with_embedded_nul(TEST_DRIVER_DIR));

    const auto loaded = DriverLoader::load(settings);

    ASSERT_FALSE(loaded);
    EXPECT_EQ(loaded.error().code, plugin::DriverLoadError::Code::INVALID_SETTINGS);
    EXPECT_EQ(loaded.error().message, "driver_dir contains an embedded NUL byte");
}

TEST(DriverLoaderTest, ManualLoadFailure_StopsAtTheFirstBadDriver) {
    auto settings = make_load_settings("simple/simple.so");
    settings.load.emplace_back("nonexistent_driver.so");

    const auto loaded = DriverLoader::load(settings);

    ASSERT_FALSE(loaded);
    EXPECT_EQ(loaded.error().code, plugin::DriverLoadError::Code::LOAD_FAILED);
}

TEST(DriverLoaderTest, TheSameDriverListedTwice_LoadsOnePackage) {
    auto settings = make_load_settings("simple/simple.so");
    settings.load.emplace_back("simple/simple.so");

    const auto loaded = DriverLoader::load(settings);

    ASSERT_TRUE(loaded) << loaded.error().message;
    EXPECT_EQ(loaded->packages.size(), 1u);
}

// ===========================================================================
//  DriverCatalog
// ===========================================================================

TEST(DriverCatalogTest, UnloadDriver) {
    const auto loaded = DriverLoader::load(make_load_settings("simple/simple.so"));
    ASSERT_TRUE(loaded) << loaded.error().message;

    DriverCatalog catalog;
    register_all(catalog, *loaded);

    // Verify it's loaded.
    ASSERT_EQ(catalog.loaded_drivers().size(), 1u);

    // Unload it.
    EXPECT_NO_THROW({ catalog.unload_driver("simple"); });
    EXPECT_TRUE(catalog.loaded_drivers().empty());
}

TEST(DriverCatalogTest, UnloadDriver_NotFound_Throws) {
    DriverCatalog catalog;
    EXPECT_THROW({ catalog.unload_driver("nonexistent"); }, DriverNotFoundException);
}

TEST(DriverCatalogTest, Find_NotFound_ReturnsNullptr) {
    const DriverCatalog catalog;
    EXPECT_EQ(catalog.find("nonexistent"), nullptr);
}

TEST(DriverCatalogTest, RegisteringTheSameDriverTwice_KeepsTheFirstPackage) {
    const auto loaded = DriverLoader::load(make_load_settings("simple/simple.so"));
    ASSERT_TRUE(loaded) << loaded.error().message;
    ASSERT_EQ(loaded->packages.size(), 1u);

    DriverCatalog catalog;
    catalog.register_package(loaded->packages.front());
    // A duplicate name keeps the first package instead of replacing it.
    catalog.register_package(loaded->packages.front());

    const auto names = catalog.loaded_drivers();
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "simple");
}

// ===========================================================================
//  Auto-discovery resilience (foreign/broken libraries are skipped)
// ===========================================================================

TEST(DriverLoaderTest, AutoDiscover_NonExistentDir_ReportsWarning) {
    domain::DriverSettings settings;
    settings.auto_discover = true;
    settings.driver_dir = "/nonexistent_directory_for_testing";

    // A missing directory is a warning, not a failed load.
    const auto loaded = DriverLoader::load(settings);

    ASSERT_TRUE(loaded);
    EXPECT_TRUE(loaded->packages.empty());
    EXPECT_TRUE(loaded->skipped.empty());
    ASSERT_EQ(loaded->warnings.size(), 1u);
    EXPECT_NE(loaded->warnings.front().find("/nonexistent_directory_for_testing"), std::string::npos)
        << loaded->warnings.front();
}

TEST(DriverLoaderTest, AutoDiscover_FileInsteadOfDirectory_ReportsWarning) {
    char path_template[] = "/tmp/yaddnsc_driver_file_XXXXXX";
    const auto fd = ::mkstemp(path_template);
    ASSERT_GE(fd, 0) << "mkstemp failed";
    ::close(fd);

    domain::DriverSettings settings;
    settings.auto_discover = true;
    settings.driver_dir = path_template;

    const auto loaded = DriverLoader::load(settings);

    ASSERT_TRUE(loaded);
    EXPECT_TRUE(loaded->packages.empty());
    EXPECT_TRUE(loaded->skipped.empty());
    ASSERT_EQ(loaded->warnings.size(), 1u);
    EXPECT_NE(loaded->warnings.front().find(path_template), std::string::npos) << loaded->warnings.front();

    ::unlink(path_template);
}

TEST(DriverLoaderTest, LoadByAbsolutePath) {
    domain::DriverSettings settings;
    settings.auto_discover = false;
    // Use absolute path.
    settings.load.push_back(std::string(TEST_DRIVER_DIR) + "/simple/simple.so");

    const auto loaded = DriverLoader::load(settings);

    ASSERT_TRUE(loaded) << loaded.error().message;
    ASSERT_EQ(loaded->packages.size(), 1u);

    DriverCatalog catalog;
    register_all(catalog, *loaded);
    const auto names = catalog.loaded_drivers();
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "simple");
}

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
        std::ofstream out(std::string(dir) + "/not_elf.so");
        ASSERT_TRUE(out.is_open());
        out << "this is not a shared library";
    }
    std::filesystem::copy_file(BAD_DRIVER_FIXTURE, std::string(dir) + "/foreign.so");

    domain::DriverSettings settings;
    settings.auto_discover = true;
    settings.driver_dir = dir;

    // Foreign libraries must be skipped, not abort the whole load.
    const auto loaded = DriverLoader::load(settings);

    ASSERT_TRUE(loaded);
    ASSERT_EQ(loaded->packages.size(), 1u);

    DriverCatalog catalog;
    register_all(catalog, *loaded);
    const auto names = catalog.loaded_drivers();
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "simple");

    // Both unusable libraries are reported as skips; the load itself succeeded.
    std::vector<plugin::DriverLoadError::Code> codes;
    codes.reserve(loaded->skipped.size());
    for (const auto& skipped : loaded->skipped) {
        codes.push_back(skipped.code);
    }
    std::ranges::sort(codes);
    const std::vector<plugin::DriverLoadError::Code> expected_codes{plugin::DriverLoadError::Code::LOAD_FAILED,
                                                                    plugin::DriverLoadError::Code::MISSING_SYMBOL};
    EXPECT_EQ(codes, expected_codes);
    EXPECT_TRUE(loaded->warnings.empty());

    std::filesystem::remove_all(dir);
}

TEST(DriverLoaderTest, AutoDiscover_IgnoresNonSharedFiles) {
    char dir_template[] = "/tmp/yaddnsc_driver_test_XXXXXX";
    auto* dir = ::mkdtemp(dir_template);
    ASSERT_NE(dir, nullptr) << "mkdtemp failed";

    std::filesystem::copy_file(std::string(TEST_DRIVER_DIR) + "/simple/simple.so", std::string(dir) + "/simple.so");
    {
        std::ofstream out(std::string(dir) + "/README.txt");
        ASSERT_TRUE(out.is_open());
        out << "not a driver";
    }
    std::filesystem::create_directory(std::string(dir) + "/nested.so");

    domain::DriverSettings settings;
    settings.auto_discover = true;
    settings.driver_dir = dir;

    const auto loaded = DriverLoader::load(settings);

    ASSERT_TRUE(loaded);
    ASSERT_EQ(loaded->packages.size(), 1u);
    // Files that are not loadable drivers are not even candidates: they are
    // neither loaded nor reported as a skip.
    EXPECT_TRUE(loaded->skipped.empty());

    DriverCatalog catalog;
    register_all(catalog, *loaded);
    EXPECT_EQ(catalog.loaded_drivers().size(), 1u);
    EXPECT_EQ(catalog.loaded_drivers()[0], "simple");

    std::filesystem::remove_all(dir);
}

TEST(DriverLoaderTest, AutoDiscover_AllBad_ReportsSkipsOnly) {
    char dir_template[] = "/tmp/yaddnsc_driver_test_XXXXXX";
    auto* dir = ::mkdtemp(dir_template);
    ASSERT_NE(dir, nullptr) << "mkdtemp failed";

    std::filesystem::copy_file(BAD_DRIVER_FIXTURE, std::string(dir) + "/foreign.so");

    domain::DriverSettings settings;
    settings.auto_discover = true;
    settings.driver_dir = dir;

    const auto loaded = DriverLoader::load(settings);

    ASSERT_TRUE(loaded);
    EXPECT_TRUE(loaded->packages.empty());
    ASSERT_EQ(loaded->skipped.size(), 1u);
    EXPECT_EQ(loaded->skipped.front().code, plugin::DriverLoadError::Code::MISSING_SYMBOL);

    std::filesystem::remove_all(dir);
}

TEST(DriverLoaderTest, AutoDiscover_ReportsThatTheManualLoadListIsIgnored) {
    char dir_template[] = "/tmp/yaddnsc_driver_test_XXXXXX";
    auto* dir = ::mkdtemp(dir_template);
    ASSERT_NE(dir, nullptr) << "mkdtemp failed";

    std::filesystem::copy_file(std::string(TEST_DRIVER_DIR) + "/simple/simple.so", std::string(dir) + "/simple.so");

    domain::DriverSettings settings;
    settings.auto_discover = true;
    settings.driver_dir = dir;
    settings.load.emplace_back("simple/simple.so");

    const auto loaded = DriverLoader::load(settings);

    ASSERT_TRUE(loaded);
    ASSERT_EQ(loaded->packages.size(), 1u);
    ASSERT_EQ(loaded->warnings.size(), 1u);
    EXPECT_NE(loaded->warnings.front().find("auto_discover"), std::string::npos) << loaded->warnings.front();

    std::filesystem::remove_all(dir);
}

// ===========================================================================
//  Catalog queries
// ===========================================================================

TEST(DriverCatalogTest, Describe_MissingDriver_ReturnsNotFound) {
    const DriverCatalog catalog;
    const auto result = catalog.describe("missing");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, domain::DriverError::Code::NOT_FOUND);
    EXPECT_TRUE(result.error().message.empty());
}

TEST(DriverCatalogTest, Describe_LoadedDriver_ReturnsOwnedDescription) {
    const auto loaded = DriverLoader::load(make_load_settings("simple/simple.so"));
    ASSERT_TRUE(loaded) << loaded.error().message;

    DriverCatalog catalog;
    register_all(catalog, *loaded);

    const auto result = catalog.describe("simple");
    ASSERT_TRUE(result);
    const auto package = catalog.find("simple");
    ASSERT_NE(package, nullptr);
    const auto& descriptor = package->descriptor();
    EXPECT_EQ(result->name, descriptor.name);
    EXPECT_EQ(result->version, descriptor.version);
    EXPECT_EQ(result->author, descriptor.author);
    EXPECT_EQ(result->description, descriptor.description);

    catalog.unload_driver("simple");
    EXPECT_EQ(result->name, "simple");
}
