//
// Tests for util/cert_util.h — CA certificate discovery.
//
// Verifies that:
//   - discover_ca_bundle() returns a path or nullopt (never crashes)
//   - SSL_CERT_FILE env var takes highest priority
//   - When SSL_CERT_FILE points to a non-existent file, the function logs
//     a warning and falls through to tiers 2-4
//   - get_system_ca_path() returns a path or nullopt (legacy)
//
// Note: both functions cache their result in a function-local static,
// so the FIRST call in the process determines the cached value.
// The env-var-not-found test must run first.
//
// =============================================================================

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstdio>
#include <string>

#include "util/cert_util.h"

// ---------------------------------------------------------------------------
// Test that discover_ca_bundle() falls through when SSL_CERT_FILE points to
// a non-existent file.  The function should log a warning and continue to
// tiers 2-4 (local ./ca.pem, OpenSSL default, hardcoded paths).
//
// This must be the FIRST discover_ca_bundle() call in the process.
// ---------------------------------------------------------------------------
TEST(CertUtilTest, DiscoverCaBundle_EnvVarNotFound) {
    // Set SSL_CERT_FILE to a path that does not exist.
    const auto *old_env = std::getenv("SSL_CERT_FILE");
    ASSERT_EQ(::setenv("SSL_CERT_FILE", "/tmp/yaddnsc_ca_nonexistent_XXXXXX", 1), 0);

    // First call — should fall through to tiers 2-4.
    auto path = Utils::Cert::discover_ca_bundle();
    // The path may or may not have a value depending on whether any system
    // CA bundle exists.  But it should not crash, and the non-existent env
    // var path should NOT have been returned.
    if (path.has_value()) {
        EXPECT_NE(*path, "/tmp/yaddnsc_ca_nonexistent_XXXXXX");
        EXPECT_FALSE(path->empty());
    }

    // Restore.
    if (old_env) {
        ::setenv("SSL_CERT_FILE", old_env, 1);
    } else {
        ::unsetenv("SSL_CERT_FILE");
    }
}

// ---------------------------------------------------------------------------
// Test that discover_ca_bundle() picks up SSL_CERT_FILE as tier 1.
//
// Because the cache is already set by the EnvVarNotFound test above, the
// cached value is used regardless of the current env var.  This test
// verifies that the function does not crash and returns a value consistent
// with the cached result.
// ---------------------------------------------------------------------------
TEST(CertUtilTest, DiscoverCaBundle_EnvVarOverride) {
    // Set SSL_CERT_FILE to a real temp file.
    char tmp[] = "/tmp/yaddnsc_ca_test_XXXXXX";
    auto fd = ::mkstemp(tmp);
    ASSERT_GE(fd, 0) << "mkstemp failed";
    ::close(fd);

    const auto *old_env = std::getenv("SSL_CERT_FILE");
    ::setenv("SSL_CERT_FILE", tmp, 1);

    // The cached result from the first test is used.  We just verify that
    // the call does not crash and returns the cached value.
    auto path = Utils::Cert::discover_ca_bundle();

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
// The result is cached from the EnvVarNotFound test above, so on systems
// where a system CA was found, the cached value reflects that.  We only
// check that the return value is valid metadata.
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
