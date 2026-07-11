//
// Tests for util/cert_util.hpp — CA certificate discovery.
//
// The test verifies that get_system_ca_path() either finds a CA bundle
// or returns nullopt.  On standard Ubuntu/Debian systems it should find
// /etc/ssl/certs/ca-certificates.crt.
//
// Note: the result is cached in a function-local static, so all tests
// in this process share the same value.
//
// =============================================================================

#include <gtest/gtest.h>

#include "util/cert_util.hpp"

TEST(CertUtilTest, GetSystemCaPath_ReturnsPathOrNullopt) {
    auto path = Utils::Cert::get_system_ca_path();
    // On Ubuntu/Debian the CA bundle is always present.
    // In minimal containers it may be absent.
    if (path.has_value()) {
        EXPECT_FALSE(path->empty());
        EXPECT_GT(path->size(), 0U);
    }
    // The function is noexcept — it should never throw.
}
