//
// Tests for util/cert_util.h — CA certificate discovery.
//
// Verifies that:
//   - discover_ca_bundle() returns a path or nullopt (never crashes)
//   - SSL_CERT_FILE env var takes highest priority
//   - get_system_ca_path() returns a path or nullopt (legacy)
//
// Note: both functions cache their result in a function-local static,
// so the FIRST call in the process determines the cached value.
// The SSL_CERT_FILE override test must run first.
//
// =============================================================================

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstdio>
#include <string>

#include "util/cert_util.h"

// ---------------------------------------------------------------------------
// Test that discover_ca_bundle() picks up SSL_CERT_FILE as tier 1.
//
// This must be the FIRST discover_ca_bundle() call in the process to avoid
// interference from the function-local cache.
// ---------------------------------------------------------------------------
TEST(CertUtilTest, DiscoverCaBundle_EnvVarOverride) {
    // Create a temporary file to simulate a CA bundle.
    char tmp[] = "/tmp/yaddnsc_ca_test_XXXXXX";
    auto fd = ::mkstemp(tmp);
    ASSERT_GE(fd, 0) << "mkstemp failed";
    ::close(fd);

    // Save current SSL_CERT_FILE and override with our temp file.
    const auto *old_env = std::getenv("SSL_CERT_FILE");
    ASSERT_EQ(::setenv("SSL_CERT_FILE", tmp, 1), 0);

    // First call to discover_ca_bundle() — should pick up the env var.
    auto path = Utils::Cert::discover_ca_bundle();
    ASSERT_TRUE(path.has_value());
    EXPECT_EQ(*path, tmp);

    // Restore the original environment (no effect on cached value).
    if (old_env) {
        ::setenv("SSL_CERT_FILE", old_env, 1);
    } else {
        ::unsetenv("SSL_CERT_FILE");
    }

    ::unlink(tmp);
}

// ---------------------------------------------------------------------------
// Basic sanity: discover_ca_bundle() should never crash and, on systems that
// have a CA bundle, should return a non-empty path.
//
// The result is cached from the EnvVarOverride test above, so on systems
// where the env var was set, this might return the temp file path (which no
// longer exists).  We only check that the return value is valid metadata.
// ---------------------------------------------------------------------------
TEST(CertUtilTest, DiscoverCaBundle_Basic) {
    auto path = Utils::Cert::discover_ca_bundle();
    // In minimal containers (e.g. CI) there may be no CA bundle.
    if (path.has_value()) {
        EXPECT_FALSE(path->empty());
    }
}

// ---------------------------------------------------------------------------
// Legacy get_system_ca_path() — backward-compatible test.
// ---------------------------------------------------------------------------
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
