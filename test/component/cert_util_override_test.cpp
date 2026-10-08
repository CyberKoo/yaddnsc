// The first discover_ca_bundle() call must observe the explicit environment
// override, even when GoogleTest repeats this case or shuffles other cases.

#include <fstream>

#include <gtest/gtest.h>

#include "infrastructure/net/tls/cert_util.h"

#include "process_test_support.h"

TEST(CertUtilEnvOverrideTest, DiscoverCaBundle_EnvVarHit) {
    if (ComponentTest::run_in_cold_process(YADDNSC_TEST_BINARY, "CertUtilEnvOverrideTest.DiscoverCaBundle_EnvVarHit")) {
        return;
    }
    const ComponentTest::TempDirectory directory("/tmp/yaddnsc_ca_hit_XXXXXX");
    const auto file = (directory.path() / "cert.pem").string();
    {
        std::ofstream out(file);
        out << "dummy CA bundle\n";
        ASSERT_TRUE(out.good());
    }
    const ComponentTest::ScopedEnvVar env("SSL_CERT_FILE", file.c_str());
    const auto path = Utils::Cert::discover_ca_bundle();
    ASSERT_TRUE(path.has_value());
    EXPECT_EQ(*path, file);
}

TEST(CertUtilEnvOverrideTest, GetSystemCaPath_StillWorks) {
    const auto path = Utils::Cert::get_system_ca_path();
    if (path.has_value()) {
        EXPECT_FALSE(path->empty());
    }
}
