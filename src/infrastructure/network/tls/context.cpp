//
// net — TlsContext implementation.
//

#include "context.h"

#include <algorithm>
#include <charconv>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <expected>
#include <openssl/prov_ssl.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <spdlog/spdlog.h>

#include "infrastructure/network/tls/cert_util.h"
#include "infrastructure/network/tls/openssl_error.hpp"
#include "infrastructure/network/tls/trust_store.h"
#include "infrastructure/network/transport/options.h"

namespace net {
namespace {

/// The bundle to trust: the explicit path when set, otherwise discovery. Returns
/// nullopt when no usable bundle exists; the caller then eagerly loads the
/// OpenSSL default cert dir (the legacy fallback) before failing closed.
[[nodiscard]] std::optional<std::string> resolve_ca_bundle(const TlsTrustOptions& options) {
    if (options.ca_bundle) {
        return options.ca_bundle;
    }
    return Utils::Cert::discover_ca_bundle();
}

}  // namespace

std::expected<void, IoError> detail::load_trust_directory(X509_STORE& store, const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::directory_iterator entry{directory, error};
    const std::filesystem::directory_iterator end;
    if (error) {
        return std::unexpected(IoError::CONNECTION_FAILED);
    }

    struct IndexedFile {
        std::string prefix;
        std::uint64_t index;
        unsigned long hash;
        bool crl;
        std::filesystem::path path;
    };

    std::vector<IndexedFile> files;
    for (; entry != end; entry.increment(error)) {
        if (error) {
            return std::unexpected(IoError::CONNECTION_FAILED);
        }
        const std::string name = entry->path().filename().string();
        // OpenSSL's hash entries: hhhhhhhh.N (cert) or hhhhhhhh.rN (CRL).
        if (name.size() < 10 || name[8] != '.' || !std::all_of(name.begin(), name.begin() + 8, [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            })) {
            continue;
        }
        const std::string_view suffix{name.data() + 9, name.size() - 9};
        const auto digits = suffix.starts_with('r') ? suffix.substr(1) : suffix;
        if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            continue;
        }
        std::uint64_t index = 0;
        const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), index);
        if (parsed.ec != std::errc{} || (digits.size() > 1 && digits.front() == '0')) {
            continue;
        }
        unsigned long hash = 0;
        std::from_chars(name.data(), name.data() + 8, hash, 16);
        files.push_back(
            {name.substr(0, suffix.starts_with('r') ? 10 : 9), index, hash, suffix.starts_with('r'), entry->path()});
    }
    if (error) {
        return std::unexpected(IoError::CONNECTION_FAILED);
    }
    std::ranges::sort(files, [](const IndexedFile& left, const IndexedFile& right) {
        return left.prefix == right.prefix ? left.index < right.index : left.prefix < right.prefix;
    });
    std::string prefix;
    std::uint64_t expected_index = 0;
    bool certificate_loaded = false;
    for (const auto& file : files) {
        if (file.prefix != prefix) {
            prefix = file.prefix;
            expected_index = 0;
        }
        // Hash lookup stops at the first missing suffix; preserve that boundary.
        if (file.index != expected_index) {
            continue;
        }
        ++expected_index;
        // Stage each eager load before admitting trust: a syntactically indexed
        // file with another subject/issuer hash must not expand the snapshot.
        // The OpenSSL loader preserves AUX and CRLs; symlinks are followed.
        const std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)> staged{X509_STORE_new(), &X509_STORE_free};
        if (!std::filesystem::is_regular_file(file.path, error) || error || !staged ||
            X509_STORE_load_file(staged.get(), file.path.c_str()) != 1) {
            return std::unexpected(IoError::CONNECTION_FAILED);
        }
        const auto* objects = X509_STORE_get0_objects(staged.get());
        for (int i = 0; i < sk_X509_OBJECT_num(objects); ++i) {
            auto* object = sk_X509_OBJECT_value(objects, i);
            if (!file.crl && X509_OBJECT_get_type(object) == X509_LU_X509) {
                X509* cert = X509_OBJECT_get0_X509(object);
                if (X509_subject_name_hash(cert) != file.hash || X509_STORE_add_cert(&store, cert) != 1) {
                    return std::unexpected(IoError::CONNECTION_FAILED);
                }
                certificate_loaded = true;
            } else if (file.crl && X509_OBJECT_get_type(object) == X509_LU_CRL) {
                X509_CRL* crl = X509_OBJECT_get0_X509_CRL(object);
                if (X509_NAME_hash(X509_CRL_get_issuer(crl)) != file.hash || X509_STORE_add_crl(&store, crl) != 1) {
                    return std::unexpected(IoError::CONNECTION_FAILED);
                }
            } else {
                return std::unexpected(IoError::CONNECTION_FAILED);
            }
        }
    }
    if (certificate_loaded) {
        return {};
    }
    return std::unexpected(IoError::CONNECTION_FAILED);
}

TlsContext::TlsContext(ConstructionKey, std::shared_ptr<SSL_CTX> context) : context_(std::move(context)) {}

std::expected<std::shared_ptr<const TlsContext>, IoError> TlsContext::create(const TlsTrustOptions& options) {
    // A path with an embedded NUL would be silently truncated by c_str().
    if (options.ca_bundle && options.ca_bundle->find('\0') != std::string::npos) {
        SPDLOG_ERROR("TLS CA bundle path contains a NUL byte");
        return std::unexpected(IoError::CONNECTION_FAILED);
    }

    std::shared_ptr<SSL_CTX> context{SSL_CTX_new(TLS_client_method()), &SSL_CTX_free};
    if (!context) {
        SPDLOG_ERROR("SSL_CTX_new failed: {}", detail::ssl_errors());
        return std::unexpected(IoError::CONNECTION_FAILED);
    }
    if (SSL_CTX_set_min_proto_version(context.get(), TLS1_2_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(context.get(), TLS1_3_VERSION) != 1) {
        SPDLOG_ERROR("Failed to restrict TLS versions: {}", detail::ssl_errors());
        return std::unexpected(IoError::CONNECTION_FAILED);
    }

    if (!options.verify_peer) {
        SSL_CTX_set_verify(context.get(), SSL_VERIFY_NONE, nullptr);
        return std::shared_ptr<const TlsContext>(new TlsContext(ConstructionKey{}, std::move(context)));
    }

    SSL_CTX_set_verify(context.get(), SSL_VERIFY_PEER, nullptr);
    const std::optional<std::string> bundle = resolve_ca_bundle(options);
    if (bundle) {
        if (SSL_CTX_load_verify_locations(context.get(), bundle->c_str(), nullptr) != 1) {
            SPDLOG_ERROR("Failed to load CA bundle from {}: {}", *bundle, detail::ssl_errors());
            return std::unexpected(IoError::CONNECTION_FAILED);
        }
    } else {
        X509_STORE* store = SSL_CTX_get_cert_store(context.get());
        if (store == nullptr || !detail::load_trust_directory(*store, X509_get_default_cert_dir())) {
            SPDLOG_ERROR("No usable TLS trust snapshot available; verification stays fail-closed");
            return std::unexpected(IoError::CONNECTION_FAILED);
        }
        SPDLOG_WARN("No TLS CA bundle discovered; snapshotted the OpenSSL default cert dir instead");
    }

    return std::shared_ptr<const TlsContext>(new TlsContext(ConstructionKey{}, std::move(context)));
}

}  // namespace net
