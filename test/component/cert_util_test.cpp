// CA discovery caches its first result. Each environment-sensitive scenario
// runs in a fresh executable image, independently of shuffle and repeat.

#include "infrastructure/network/tls/cert_util.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>

#include <gtest/gtest.h>
#include <openssl/objects.h>
#include <openssl/pem.h>
#include <openssl/x509_vfy.h>

#include "infrastructure/network/tls/trust_store.h"

#include "process_test_support.h"
#include "trust_test_support.h"

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

using ComponentTest::make_certificate;
using ComponentTest::write_indexed_certificate;

TEST(TlsTrustSnapshot, HashDirectory_LoadsAllIssuersAndAuxBeforeSourcesDisappear) {
    const ComponentTest::TempDirectory directory("/tmp/yaddnsc_trust_snapshot_XXXXXX");
    auto first = make_certificate("snapshot issuer one");
    auto second = make_certificate("snapshot issuer two");
    ASSERT_EQ(X509_add1_trust_object(first.get(), OBJ_nid2obj(NID_client_auth)), 1);
    ASSERT_EQ(X509_add1_reject_object(first.get(), OBJ_nid2obj(NID_server_auth)), 1);
    write_indexed_certificate(directory.path(), *first);
    write_indexed_certificate(directory.path(), *first, 1);  // duplicates remain harmless
    write_indexed_certificate(directory.path(), *second);
    const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> crl_key{
        EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"), &EVP_PKEY_free};
    const std::unique_ptr<X509_CRL, decltype(&X509_CRL_free)> crl{X509_CRL_new(), &X509_CRL_free};
    ASSERT_TRUE(crl_key);
    ASSERT_TRUE(crl);
    ASSERT_EQ(X509_CRL_set_version(crl.get(), 1), 1);
    ASSERT_EQ(X509_CRL_set_issuer_name(crl.get(), X509_get_subject_name(first.get())), 1);
    const std::unique_ptr<ASN1_TIME, decltype(&ASN1_TIME_free)> now{ASN1_TIME_new(), &ASN1_TIME_free};
    ASSERT_TRUE(now);
    ASN1_TIME_set(now.get(), std::time(nullptr));
    ASSERT_EQ(X509_CRL_set1_lastUpdate(crl.get(), now.get()), 1);
    ASSERT_GT(X509_CRL_sign(crl.get(), crl_key.get(), EVP_sha256()), 0);
    char crl_name[32]{};
    std::snprintf(crl_name, sizeof(crl_name), "%08lx.r0", X509_subject_name_hash(first.get()));
    {
        const std::unique_ptr<BIO, decltype(&BIO_free)> file{BIO_new_file((directory.path() / crl_name).c_str(), "w"),
                                                             &BIO_free};
        ASSERT_TRUE(file);
        ASSERT_EQ(PEM_write_bio_X509_CRL(file.get(), crl.get()), 1);
    }
    const std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)> store{X509_STORE_new(), &X509_STORE_free};
    ASSERT_TRUE(store);
    ASSERT_TRUE(net::detail::load_trust_directory(*store, directory.path()));
    std::filesystem::remove_all(directory.path());
    const std::unique_ptr<X509_STORE_CTX, decltype(&X509_STORE_CTX_free)> context{X509_STORE_CTX_new(),
                                                                                  &X509_STORE_CTX_free};
    ASSERT_TRUE(context);
    ASSERT_EQ(X509_STORE_CTX_init(context.get(), store.get(), second.get(), nullptr), 1);
    EXPECT_EQ(X509_verify_cert(context.get()), 1);
    const auto* objects = X509_STORE_get0_objects(store.get());
    EXPECT_EQ(sk_X509_OBJECT_num(objects), 3);
    bool aux_found = false;
    for (int i = 0; i < sk_X509_OBJECT_num(objects); ++i) {
        auto* object = sk_X509_OBJECT_value(objects, i);
        if (X509_OBJECT_get_type(object) != X509_LU_X509) {
            EXPECT_EQ(X509_OBJECT_get_type(object), X509_LU_CRL);
            continue;
        }
        X509* cert = X509_OBJECT_get0_X509(object);
        if (X509_NAME_cmp(X509_get_subject_name(cert), X509_get_subject_name(first.get())) == 0) {
            aux_found = true;
            EXPECT_EQ(sk_ASN1_OBJECT_num(X509_get0_trust_objects(cert)), 1);
            EXPECT_EQ(sk_ASN1_OBJECT_num(X509_get0_reject_objects(cert)), 1);
        }
    }
    EXPECT_TRUE(aux_found);
}

TEST(TlsTrustSnapshot, MissingEmptyOrMalformedIndexedSource_FailsClosed) {
    const ComponentTest::TempDirectory directory("/tmp/yaddnsc_trust_invalid_XXXXXX");
    const std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)> store{X509_STORE_new(), &X509_STORE_free};
    ASSERT_TRUE(store);
    EXPECT_FALSE(net::detail::load_trust_directory(*store, directory.path() / "missing"));
    EXPECT_FALSE(net::detail::load_trust_directory(*store, directory.path()));
    {
        std::ofstream out(directory.path() / "12345678.0");
        out << "malformed PEM";
    }
    EXPECT_FALSE(net::detail::load_trust_directory(*store, directory.path()));
}

TEST(TlsTrustSnapshot, HashSuffixGap_DoesNotIntroduceUnreachableTrustEntries) {
    const ComponentTest::TempDirectory directory("/tmp/yaddnsc_trust_gap_XXXXXX");
    auto cert = make_certificate("gap issuer");
    write_indexed_certificate(directory.path(), *cert, 1);
    const std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)> store{X509_STORE_new(), &X509_STORE_free};
    ASSERT_TRUE(store);
    EXPECT_FALSE(net::detail::load_trust_directory(*store, directory.path()));
    EXPECT_EQ(sk_X509_OBJECT_num(X509_STORE_get0_objects(store.get())), 0);
}

TEST(TlsTrustSnapshot, BadIndexedEntryAlongsideValidRoot_RejectsWholeSnapshot) {
    for (int kind = 0; kind < 3; ++kind) {
        const ComponentTest::TempDirectory directory("/tmp/yaddnsc_trust_bad_entry_XXXXXX");
        auto cert = make_certificate("usable root beside bad entry");
        write_indexed_certificate(directory.path(), *cert);
        const auto bad = directory.path() / "12345678.0";
        if (kind == 0) {
            std::filesystem::create_symlink(directory.path() / "missing.pem", bad);
        } else if (kind == 1) {
            std::filesystem::create_directory(bad);
        } else {
            std::ofstream{bad} << "malformed PEM";
        }
        const std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)> store{X509_STORE_new(), &X509_STORE_free};
        ASSERT_TRUE(store);
        EXPECT_FALSE(net::detail::load_trust_directory(*store, directory.path()));
    }
}

TEST(TlsTrustSnapshot, MisnamedCertificate_DoesNotExpandTrust) {
    const ComponentTest::TempDirectory directory("/tmp/yaddnsc_trust_wrong_hash_XXXXXX");
    auto cert = make_certificate("misnamed root");
    write_indexed_certificate(directory.path(), *cert);
    const auto source = std::filesystem::directory_iterator(directory.path())->path();
    const auto wrong = directory.path() / (source.filename() == "00000000.0" ? "00000001.0" : "00000000.0");
    std::filesystem::rename(source, wrong);
    const std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)> store{X509_STORE_new(), &X509_STORE_free};
    ASSERT_TRUE(store);
    EXPECT_FALSE(net::detail::load_trust_directory(*store, directory.path()));
    EXPECT_EQ(sk_X509_OBJECT_num(X509_STORE_get0_objects(store.get())), 0);
}

TEST(TlsTrustSnapshot, EmptyDirectory_IsRejectedEvenWhenDestinationAlreadyContainsTrust) {
    const ComponentTest::TempDirectory directory("/tmp/yaddnsc_trust_prepopulated_XXXXXX");
    auto cert = make_certificate("existing trust");
    const std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)> store{X509_STORE_new(), &X509_STORE_free};
    ASSERT_TRUE(store);
    ASSERT_EQ(X509_STORE_add_cert(store.get(), cert.get()), 1);
    EXPECT_FALSE(net::detail::load_trust_directory(*store, directory.path()));
}
