#ifndef YADDNSC_INFRASTRUCTURE_NETWORK_TLS_TRUST_STORE_H
#define YADDNSC_INFRASTRUCTURE_NETWORK_TLS_TRUST_STORE_H

#include <filesystem>

#include <expected>
#include <openssl/types.h>

#include "infrastructure/network/transport/io_error.h"

namespace net::detail {

/// Eagerly snapshot PEM certificates/CRLs from OpenSSL hash-directory entries.
/// Performs filesystem I/O; call before the loop or on a worker. No directory
/// lookup is installed. Missing/empty sources, unreadable/malformed indexed
/// entries or subject/issuer hashes inconsistent with their names fail closed.
/// Unlike lazy hash lookup, one bad indexed entry rejects the whole snapshot.
/// Allocation failures propagate; discard the mutable store on load failure.
[[nodiscard]] std::expected<void, IoError> load_trust_directory(X509_STORE& store,
                                                                const std::filesystem::path& directory);

}  // namespace net::detail

#endif  // YADDNSC_INFRASTRUCTURE_NETWORK_TLS_TRUST_STORE_H
