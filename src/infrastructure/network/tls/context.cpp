//
// net — TlsContext implementation.
//

#include "context.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <openssl/ssl.h>

#include <spdlog/spdlog.h>

#include "infrastructure/network/tls/openssl_error.hpp"
#include "infrastructure/network/tls/cert_util.h"

namespace net {
namespace {

/// The bundle to trust: the explicit path when set, otherwise discovery. Returns
/// nullopt when no usable bundle exists; the caller then eagerly loads the
/// OpenSSL default cert dir (the legacy fallback) before failing closed.
[[nodiscard]] std::optional<std::string> resolve_ca_bundle(const TlsOptions& options) {
    if (options.ca_bundle) {
        return options.ca_bundle;
    }
    return Utils::Cert::discover_ca_bundle();
}

}  // namespace

TlsContext::TlsContext(ConstructionKey, std::shared_ptr<SSL_CTX> context) : context_(std::move(context)) {}

std::expected<std::shared_ptr<const TlsContext>, IoError> TlsContext::create(const TlsOptions& options) {
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
    } else if (X509_STORE* store = SSL_CTX_get_cert_store(context.get());
               X509_STORE_load_path(store, X509_get_default_cert_dir()) == 1) {
        // Legacy fallback: no bundle file was discovered, but the OpenSSL
        // default cert dir (a hashed dir without the bundle file — some
        // minimal containers) may still verify. X509_STORE_load_path loads
        // eagerly, so the loop thread never touches the filesystem mid-
        // handshake; the lazy lookup registrars stay banned (rule 13).
        SPDLOG_WARN("No TLS CA bundle discovered; loaded the OpenSSL default cert dir instead");
    } else {
        SPDLOG_ERROR("No TLS CA bundle available; verification stays fail-closed");
        return std::unexpected(IoError::CONNECTION_FAILED);
    }

    return std::shared_ptr<const TlsContext>(new TlsContext(ConstructionKey{}, std::move(context)));
}

}  // namespace net
