#ifndef YADDNSC_TEST_COMPONENT_TRUST_TEST_SUPPORT_H
#define YADDNSC_TEST_COMPONENT_TRUST_TEST_SUPPORT_H

#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>

#include <gtest/gtest.h>
#include <openssl/opensslv.h>
#include <openssl/pem.h>
#include <openssl/safestack.h>
#include <openssl/x509.h>

namespace ComponentTest {

using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;

#if OPENSSL_VERSION_NUMBER >= 0x30300000L
inline void free_x509_objects(STACK_OF(X509_OBJECT)* objects) {
    sk_X509_OBJECT_pop_free(objects, X509_OBJECT_free);
}
#endif

template <typename Visitor>
inline void with_store_objects(X509_STORE* store, Visitor visitor) {
#if OPENSSL_VERSION_NUMBER >= 0x30300000L
    const std::unique_ptr<STACK_OF(X509_OBJECT), decltype(&free_x509_objects)> objects{
        X509_STORE_get1_objects(store), &free_x509_objects};
    ASSERT_TRUE(objects);
    visitor(objects.get());
#else
    visitor(X509_STORE_get0_objects(store));
#endif
}

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
    const std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)> subject{X509_NAME_new(), &X509_NAME_free};
    if (!subject) {
        throw std::runtime_error("certificate subject allocation failed");
    }
    X509_NAME_add_entry_by_txt(subject.get(), "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(name), -1, -1,
                               0);
    X509_set_subject_name(cert.get(), subject.get());
    X509_set_issuer_name(cert.get(), subject.get());
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
