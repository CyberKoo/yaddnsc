//
// Capability-bit translation in the driver catalog
// (src/infrastructure/plugin/driver_catalog.cpp, DriverCatalog::describe).
//
// The descriptor's YADDNSC_DRIVER_CAPABILITY_* bits are translated into the
// record kinds the application layer sees, so that layer never handles an ABI
// number. Two real driver plugins carry one bit each and one carries none, so
// each arm of the translation is driven by an actual loaded module rather than
// by a synthetic descriptor.
//
// Both bits are independent: a plugin that resolves AAAA but not A must not
// gain a default A, and a plugin that declares no bits must report no record
// kinds at all rather than falling back to one.
//
// The catalog only registers packages: the loader discovers and validates
// them and reports what it could not load. The driver name in a load failure
// is taken from the file path, so the slash-less-path arm is driven here too.
//

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "application/ports/driver_catalog.h"
#include "domain/config/runtime_config.h"
#include "domain/dns/record_kind.h"
#include "domain/error/error.h"
#include "infrastructure/plugin/driver_catalog.h"
#include "infrastructure/plugin/driver_loader.h"
#include "infrastructure/plugin/plugin_package.h"

namespace {

/// Settings that load one driver image addressed by its file name inside a
/// driver directory.
[[nodiscard]] domain::DriverSettings make_settings_in(const std::filesystem::path& base_dir,
                                                      std::string_view file_name) {
    domain::DriverSettings settings;
    settings.auto_discover = false;
    settings.driver_dir = base_dir;
    settings.load.emplace_back(file_name);
    return settings;
}

/// Settings that load one driver image addressed by its full path.
[[nodiscard]] domain::DriverSettings make_settings_for(const std::string& path) {
    const std::filesystem::path driver_path{path};
    const std::string file_name = driver_path.filename().string();
    return make_settings_in(driver_path.parent_path(), file_name);
}

/// Load one driver image and register its package in the catalog.
void load_into(DriverCatalog& catalog, const std::string& path) {
    const auto loaded = DriverLoader::load(make_settings_for(path));
    ASSERT_TRUE(loaded) << loaded.error().message;
    for (const auto& package : loaded->packages) {
        catalog.register_package(package);
    }
}

}  // namespace

TEST(DriverCatalogCapabilities, AaaaOnlyPlugin_ReportsAaaaAndNeverADefaultA) {
    DriverCatalog catalog;
    load_into(catalog, AAAA_ONLY_FIXTURE);

    const auto result = catalog.describe("aaaa_only");

    ASSERT_TRUE(result) << result.error().message;
    EXPECT_TRUE(result->capabilities.supports(domain::RecordKind::AAAA));
    EXPECT_FALSE(result->capabilities.supports(domain::RecordKind::A))
        << "an AAAA-only plugin must not gain an A record kind it never declared";
    EXPECT_EQ(result->capabilities.supported_records().size(), 1U);
    EXPECT_EQ(result->name, "aaaa_only");
}

TEST(DriverCatalogCapabilities, PluginWithNoCapabilityBits_ReportsNoRecordKinds) {
    // The empty set is the honest answer: the gate in the gateway refuses
    // every record type for such a plugin, which is the intended behaviour
    // rather than a silent default to A.
    DriverCatalog catalog;
    load_into(catalog, NO_CAPS_FIXTURE);

    const auto result = catalog.describe("no_caps");

    ASSERT_TRUE(result) << result.error().message;
    EXPECT_TRUE(result->capabilities.supported_records().empty());
    EXPECT_EQ(result->name, "no_caps");
}

TEST(DriverCatalogCapabilities, DescribeOfAnUnloadedDriver_IsNotFoundRatherThanEmpty) {
    // A name that was never loaded must not be confused with a loaded plugin
    // that declares no records: one is an error, the other a description.
    DriverCatalog catalog;
    load_into(catalog, NO_CAPS_FIXTURE);

    const auto result = catalog.describe("aaaa_only");  // never loaded here

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, domain::DriverError::Code::NOT_FOUND);
}

TEST(DriverCatalogCapabilities, LoadFailureNamesTheFileWhenThePathHasNoDirectory) {
    // The driver-facing message uses the bare file name. A relative path with
    // no separator has no directory part to strip, and must still read as a
    // file name rather than losing the name altogether.
    const auto loaded =
        DriverLoader::load(make_settings_in(std::filesystem::path(NO_CAPS_FIXTURE).parent_path(), "no_such_driver.so"));

    ASSERT_FALSE(loaded);
    const std::string& message = loaded.error().message;
    EXPECT_NE(message.find("no_such_driver.so"), std::string::npos) << message;
}

TEST(DriverCatalogCapabilities, LoadFailureKeepsTheDirectoryInTheFullPath) {
    // The same message also carries the full path, so a reader can see where
    // the loader looked when several driver directories are configured.
    const auto loaded = DriverLoader::load(make_settings_in("/nonexistent/drivers", "absent.so"));

    ASSERT_FALSE(loaded);
    const std::string& message = loaded.error().message;
    EXPECT_NE(message.find("absent.so"), std::string::npos) << message;
    EXPECT_NE(message.find("/nonexistent/drivers/absent.so"), std::string::npos) << message;
    EXPECT_EQ(loaded.error().driver, "/nonexistent/drivers/absent.so");
}
