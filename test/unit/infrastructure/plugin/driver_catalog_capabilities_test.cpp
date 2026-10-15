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
// The driver name in each error is taken from the file path, so the
// slash-less-path arm is driven here too.
//

#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "domain/dns/record_kind.h"
#include "domain/error/error.h"
#include "infrastructure/plugin/driver_catalog.h"
#include "infrastructure/plugin/plugin_load_exception.h"

namespace {

[[nodiscard]] bool supports(const std::vector<domain::RecordKind>& kinds, domain::RecordKind wanted) {
    for (const auto kind : kinds) {
        if (kind == wanted) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST(DriverCatalogCapabilities, AaaaOnlyPlugin_ReportsAaaaAndNeverADefaultA) {
    DriverCatalog catalog;
    ASSERT_NO_THROW(catalog.load_driver(std::string(AAAA_ONLY_FIXTURE)));

    const auto result = catalog.describe("aaaa_only");

    ASSERT_TRUE(result) << result.error().message;
    EXPECT_TRUE(supports(result->supported_records, domain::RecordKind::AAAA));
    EXPECT_FALSE(supports(result->supported_records, domain::RecordKind::A))
        << "an AAAA-only plugin must not gain an A record kind it never declared";
    EXPECT_EQ(result->supported_records.size(), 1U);
    EXPECT_EQ(result->name, "aaaa_only");
}

TEST(DriverCatalogCapabilities, PluginWithNoCapabilityBits_ReportsNoRecordKinds) {
    // The empty set is the honest answer: the gate in the gateway refuses
    // every record type for such a plugin, which is the intended behaviour
    // rather than a silent default to A.
    DriverCatalog catalog;
    ASSERT_NO_THROW(catalog.load_driver(std::string(NO_CAPS_FIXTURE)));

    const auto result = catalog.describe("no_caps");

    ASSERT_TRUE(result) << result.error().message;
    EXPECT_TRUE(result->supported_records.empty());
    EXPECT_EQ(result->name, "no_caps");
}

TEST(DriverCatalogCapabilities, DescribeOfAnUnloadedDriver_IsNotFoundRatherThanEmpty) {
    // A name that was never loaded must not be confused with a loaded plugin
    // that declares no records: one is an error, the other a description.
    DriverCatalog catalog;
    catalog.load_driver(std::string(NO_CAPS_FIXTURE));

    const auto result = catalog.describe("aaaa_only");  // never loaded here

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, domain::DriverError::Code::NOT_FOUND);
}

TEST(DriverCatalogCapabilities, LoadFailureNamesTheFileWhenThePathHasNoDirectory) {
    // The driver-facing message uses the bare file name. A relative path with
    // no separator has no directory part to strip, and must still read as a
    // file name rather than losing the name altogether.
    try {
        [[maybe_unused]] DriverCatalog catalog;
        catalog.load_driver("no_such_driver.so");
        FAIL() << "loading a missing library must throw";
    } catch (const PluginLoadException& error) {
        const std::string message{error.what()};
        EXPECT_NE(message.find("no_such_driver.so"), std::string::npos) << message;
    }
}

TEST(DriverCatalogCapabilities, LoadFailureKeepsTheDirectoryInTheFullPath) {
    // The same message also carries the full path, so a reader can see where
    // the loader looked when several driver directories are configured.
    try {
        [[maybe_unused]] DriverCatalog catalog;
        catalog.load_driver("/nonexistent/drivers/absent.so");
        FAIL() << "loading a missing library must throw";
    } catch (const PluginLoadException& error) {
        const std::string message{error.what()};
        EXPECT_NE(message.find("absent.so"), std::string::npos) << message;
        EXPECT_NE(message.find("/nonexistent/drivers/absent.so"), std::string::npos) << message;
    }
}
