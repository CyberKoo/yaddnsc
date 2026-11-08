// Isolated link-time adapters drive the real TlsContext::create fallback;
// production discovery and its static cache are tested in cert_util_test.
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <gtest/gtest.h>
#include <openssl/ssl.h>
#include <openssl/types.h>
#include <openssl/x509.h>

#include "infrastructure/network/tls/cert_util.h"
#include "infrastructure/network/tls/context.h"
#include "infrastructure/network/transport/options.h"

#include "process_test_support.h"
#include "trust_test_support.h"

namespace {
std::string default_directory;
int directory_queries = 0;
int discovery_queries = 0;
}  // namespace

std::optional<std::string> Utils::Cert::discover_ca_bundle() {
    ++discovery_queries;
    return std::nullopt;
}

extern "C" const char* X509_get_default_cert_dir() {
    ++directory_queries;
    return default_directory.c_str();
}

TEST(TlsTrustFallback, ColdCreate_OnlyHashDirectory_SnapshotSurvivesSourceRemoval) {
    if (ComponentTest::run_in_cold_process(
            YADDNSC_TEST_BINARY, "TlsTrustFallback.ColdCreate_OnlyHashDirectory_SnapshotSurvivesSourceRemoval")) {
        return;
    }
    const ComponentTest::TempDirectory directory("/tmp/yaddnsc_trust_create_XXXXXX");
    auto certificate = ComponentTest::make_certificate("fallback root");
    ComponentTest::write_indexed_certificate(directory.path(), *certificate);
    default_directory = directory.path().string();
    const auto result = net::TlsContext::create(net::TlsTrustOptions{});
    ASSERT_TRUE(result);
    EXPECT_EQ(discovery_queries, 1);
    EXPECT_EQ(directory_queries, 1);
    EXPECT_EQ(SSL_CTX_get_verify_mode((*result)->native_handle()), SSL_VERIFY_PEER);
    std::filesystem::remove_all(directory.path());
    const std::unique_ptr<X509_STORE_CTX, decltype(&X509_STORE_CTX_free)> verifier{X509_STORE_CTX_new(),
                                                                                   &X509_STORE_CTX_free};
    ASSERT_TRUE(verifier);
    ASSERT_EQ(X509_STORE_CTX_init(verifier.get(), SSL_CTX_get_cert_store((*result)->native_handle()), certificate.get(),
                                  nullptr),
              1);
    EXPECT_EQ(X509_verify_cert(verifier.get()), 1);
}

TEST(TlsTrustFallback, ColdCreate_EmptyDirectory_FailsClosed) {
    if (ComponentTest::run_in_cold_process(YADDNSC_TEST_BINARY,
                                           "TlsTrustFallback.ColdCreate_EmptyDirectory_FailsClosed")) {
        return;
    }
    const ComponentTest::TempDirectory directory("/tmp/yaddnsc_trust_create_empty_XXXXXX");
    default_directory = directory.path().string();
    EXPECT_FALSE(net::TlsContext::create(net::TlsTrustOptions{}));
    EXPECT_EQ(discovery_queries, 1);
    EXPECT_EQ(directory_queries, 1);
}

TEST(TlsTrustFallback, ColdCreate_ExplicitCertDirectories_LoadAllWithoutDefault) {
    if (ComponentTest::run_in_cold_process(
            YADDNSC_TEST_BINARY, "TlsTrustFallback.ColdCreate_ExplicitCertDirectories_LoadAllWithoutDefault")) {
        return;
    }
    const ComponentTest::TempDirectory root("/tmp/yaddnsc_trust_env_dirs_XXXXXX");
    const auto first_dir = root.path() / "first";
    const auto second_dir = root.path() / "second";
    std::filesystem::create_directories(first_dir);
    std::filesystem::create_directories(second_dir);
    auto first = ComponentTest::make_certificate("env root one");
    auto second = ComponentTest::make_certificate("env root two");
    ComponentTest::write_indexed_certificate(first_dir, *first);
    ComponentTest::write_indexed_certificate(second_dir, *second);
    const auto directories = first_dir.string() + ":" + second_dir.string();
    const ComponentTest::ScopedEnvVar env("SSL_CERT_DIR", directories.c_str());

    const auto result = net::TlsContext::create(net::TlsTrustOptions{});
    ASSERT_TRUE(result);
    EXPECT_EQ(discovery_queries, 1);
    EXPECT_EQ(directory_queries, 0);
    for (const auto& certificate : {&first, &second}) {
        const std::unique_ptr<X509_STORE_CTX, decltype(&X509_STORE_CTX_free)> verifier{X509_STORE_CTX_new(),
                                                                                       &X509_STORE_CTX_free};
        ASSERT_TRUE(verifier);
        ASSERT_EQ(X509_STORE_CTX_init(verifier.get(), SSL_CTX_get_cert_store((*result)->native_handle()),
                                      certificate->get(), nullptr),
                  1);
        EXPECT_EQ(X509_verify_cert(verifier.get()), 1);
    }
}

TEST(TlsTrustFallback, ColdCreate_InvalidExplicitCertDirectory_FailsWithoutDefaultFallback) {
    if (ComponentTest::run_in_cold_process(
            YADDNSC_TEST_BINARY,
            "TlsTrustFallback.ColdCreate_InvalidExplicitCertDirectory_FailsWithoutDefaultFallback")) {
        return;
    }
    const ComponentTest::TempDirectory empty_dir("/tmp/yaddnsc_trust_env_empty_XXXXXX");
    const ComponentTest::TempDirectory default_dir("/tmp/yaddnsc_trust_env_default_XXXXXX");
    auto certificate = ComponentTest::make_certificate("default root");
    ComponentTest::write_indexed_certificate(default_dir.path(), *certificate);
    default_directory = default_dir.path().string();
    directory_queries = 0;
    const ComponentTest::ScopedEnvVar env("SSL_CERT_DIR", empty_dir.path().c_str());

    EXPECT_FALSE(net::TlsContext::create(net::TlsTrustOptions{}));
    EXPECT_EQ(discovery_queries, 1);
    EXPECT_EQ(directory_queries, 0);
}

TEST(TlsTrustFallback, ExplicitMissingBundle_DoesNotUseDirectoryFallback) {
    directory_queries = 0;
    discovery_queries = 0;
    net::TlsTrustOptions options;
    options.ca_bundle = "/nonexistent/yaddnsc-test-explicit-bundle.pem";
    EXPECT_FALSE(net::TlsContext::create(options));
    EXPECT_EQ(discovery_queries, 0);
    EXPECT_EQ(directory_queries, 0);
}
