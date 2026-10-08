// CA discovery caches its first result. Each environment-sensitive scenario
// runs in a fresh executable image, independently of shuffle and repeat.

#include "infrastructure/net/tls/cert_util.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "process_test_support.h"

TEST(CertUtilTest, DiscoverCaBundle_EnvVarNotFound) {
    if (ComponentTest::run_in_cold_process(YADDNSC_TEST_BINARY, "CertUtilTest.DiscoverCaBundle_EnvVarNotFound")) {
        return;
    }
    const ComponentTest::TempDirectory directory("/tmp/yaddnsc_ca_fallback_XXXXXX");
    // The exec child alone changes cwd; never truncate a user's ./ca.pem.
    std::filesystem::current_path(directory.path());
    const auto missing = (directory.path() / "missing.pem").string();
    const ComponentTest::ScopedEnvVar env("SSL_CERT_FILE", missing.c_str());
    {
        std::ofstream out("ca.pem");
        out << "not a system trust anchor\n";
        ASSERT_TRUE(out.good());
    }
    const auto path = Utils::Cert::discover_ca_bundle();
    if (path) {
        EXPECT_NE(*path, missing);
        EXPECT_NE(*path, "./ca.pem");
        EXPECT_NE(*path, "ca.pem");
        EXPECT_NE(*path, (directory.path() / "ca.pem").string());
        EXPECT_FALSE(path->empty());
    }
    // Cache stability is asserted within this cold scenario, not by test order.
    const ComponentTest::ScopedEnvVar changed_env("SSL_CERT_FILE", "ca.pem");
    EXPECT_EQ(Utils::Cert::discover_ca_bundle(), path);
}

TEST(CertUtilTest, DiscoverCaBundle_EnvVarOverride) {
    if (ComponentTest::run_in_cold_process(YADDNSC_TEST_BINARY, "CertUtilTest.DiscoverCaBundle_EnvVarOverride")) {
        return;
    }
    const ComponentTest::TempDirectory directory("/tmp/yaddnsc_ca_override_XXXXXX");
    const auto file = (directory.path() / "cert.pem").string();
    {
        std::ofstream out(file);
        out << "dummy CA bundle\n";
        ASSERT_TRUE(out.good());
    }
    const ComponentTest::ScopedEnvVar env("SSL_CERT_FILE", file.c_str());
    EXPECT_EQ(Utils::Cert::discover_ca_bundle(), std::optional<std::string>(file));
    const ComponentTest::ScopedEnvVar changed_env("SSL_CERT_FILE", "/nonexistent/ca.pem");
    EXPECT_EQ(Utils::Cert::discover_ca_bundle(), std::optional<std::string>(file));
}

TEST(CertUtilTest, DiscoverCaBundle_Basic) {
    const auto path = Utils::Cert::discover_ca_bundle();
    if (path) {
        EXPECT_FALSE(path->empty());
    }
}

TEST(CertUtilTest, GetSystemCaPath_ReturnsPathOrNullopt) {
    const auto path = Utils::Cert::get_system_ca_path();
    if (path) {
        EXPECT_FALSE(path->empty());
    }
}
