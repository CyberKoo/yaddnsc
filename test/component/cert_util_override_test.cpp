//
// Tests for util/cert_util.h — CA certificate discovery, env-var override.
//
// This is a SEPARATE test binary from cert_util_test.cpp on purpose:
// discover_ca_bundle() caches its result in a function-local static on the
// FIRST call in the process, so the "SSL_CERT_FILE points to an existing
// file" branch can only be exercised when the first call happens with the
// env var set.  The main cert_util_test.cpp makes its first call with a
// non-existent SSL_CERT_FILE, which cements the fall-through result.
//
// Here the very first (and only) discover_ca_bundle() call happens with
// SSL_CERT_FILE pointing to a real file → covers the tier-1 hit branch.
// =============================================================================

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>

#include <unistd.h>

#include "util/cert_util.h"

namespace {
    /// RAII set/unset of an environment variable.
    class ScopedEnvVar {
    public:
        explicit ScopedEnvVar(const char *name, const char *value) : name_(name) {
            old_value_ = std::getenv(name);
            old_present_ = (old_value_ != nullptr);
            ::setenv(name, value, 1);
        }

        ~ScopedEnvVar() {
            if (old_present_) {
                ::setenv(name_.c_str(), old_value_, 1);
            } else {
                ::unsetenv(name_.c_str());
            }
        }

        ScopedEnvVar(const ScopedEnvVar &) = delete;
        ScopedEnvVar &operator=(const ScopedEnvVar &) = delete;

    private:
        std::string name_;
        const char *old_value_{nullptr};
        bool old_present_{false};
    };
} // namespace

TEST(CertUtilEnvOverrideTest, DiscoverCaBundle_EnvVarHit) {
    // Create a real, non-empty CA file.
    char tmp[] = "/tmp/yaddnsc_ca_hit_XXXXXX";
    const auto fd = ::mkstemp(tmp);
    ASSERT_GE(fd, 0) << "mkstemp failed";
    ASSERT_GT(::write(fd, "dummy CA bundle\n", 16), 0);
    ::close(fd);

    {
        ScopedEnvVar env("SSL_CERT_FILE", tmp);

        // FIRST call in this process — the env var points at an existing file,
        // so tier 1 must win regardless of any system CA bundle.
        const auto path = Utils::Cert::discover_ca_bundle();
        ASSERT_TRUE(path.has_value());
        EXPECT_EQ(*path, tmp);
    }

    ::unlink(tmp);
}

TEST(CertUtilEnvOverrideTest, GetSystemCaPath_StillWorks) {
    // Independent of the env override (different function, different cache) —
    // must not crash and must return a value consistent with the system.
    const auto path = Utils::Cert::get_system_ca_path();
    if (path.has_value()) {
        EXPECT_FALSE(path->empty());
    }
}
