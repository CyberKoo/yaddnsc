#ifndef YADDNSC_TEST_COMPONENT_TRUST_TEST_SUPPORT_H
#define YADDNSC_TEST_COMPONENT_TRUST_TEST_SUPPORT_H

#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>

#include <gtest/gtest.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

namespace ComponentTest {

using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;

inline Certificate make_certificate(const char* name) {
    const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"),
                                                                  &EVP_PKEY_free};
    Certificate cert{X509_new(), &X509_free};
    if (!key || !cert) {
        throw std::runtime_error("certificate allocation failed");
    }
    X509_set_version(cert.get(), 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1);
    X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60);
    X509_gmtime_adj(X509_getm_notAfter(cert.get()), 3600);
    X509_set_pubkey(cert.get(), key.get());
    auto* subject = X509_get_subject_name(cert.get());
    X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(name), -1, -1, 0);
    X509_set_issuer_name(cert.get(), subject);
    if (X509_sign(cert.get(), key.get(), EVP_sha256()) == 0) {
        throw std::runtime_error("certificate signing failed");
    }
    return cert;
}

inline void write_indexed_certificate(const std::filesystem::path& dir, X509& cert, unsigned index = 0) {
    char filename[32]{};
    std::snprintf(filename, sizeof(filename), "%08lx.%u", X509_subject_name_hash(&cert), index);
    const auto path = dir / filename;
    const std::unique_ptr<BIO, decltype(&BIO_free)> file{BIO_new_file(path.c_str(), "w"), &BIO_free};
    ASSERT_TRUE(file);
    ASSERT_EQ(PEM_write_bio_X509_AUX(file.get(), &cert), 1);
}

}  // namespace ComponentTest

#endif
