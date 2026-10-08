//
// net — TlsContext: the immutable, pre-built OpenSSL client trust context.
//
// Building a trust context reads the filesystem (CA discovery and load), so it
// is a blocking operation and must run off the event loop: before the loop
// starts, or from coro::offload. A stream receives an already-built context and
// never loads a CA bundle during a handshake.
//

#ifndef YADDNSC_NET_TLS_CONTEXT_H
#define YADDNSC_NET_TLS_CONTEXT_H

#include <memory>

#include <expected>
#include <openssl/types.h>

#include "infrastructure/net/io_error.h"
#include "infrastructure/net/options.h"

namespace net {

/// An immutable OpenSSL client SSL_CTX plus the verification policy it was
/// built from.
///
/// Ownership: produced by create() and shared as std::shared_ptr<const
/// TlsContext>; SSL sessions borrow the handle, so the context must outlive every
/// stream built from it. Immutable once built, and OpenSSL allows independent SSL
/// sessions to share one SSL_CTX concurrently.
/// Failure: create() returns expected<..., IoError> with CONNECTION_FAILED for a
/// failed SSL_CTX build or a missing/unloadable trust store. It never falls back
/// to OpenSSL's lazy default-verify paths.
class TlsContext {
public:
    /// Build a context from @p options. Performs certificate file I/O (CA
    /// discovery and load), so it must be called off the event loop — before the
    /// loop starts or from coro::offload — never from a coroutine running on the
    /// loop thread.
    ///
    /// Fail-closed: with verify_peer set, the CA bundle is the explicit
    /// `ca_bundle` when present, otherwise Utils::Cert::discover_ca_bundle(); if
    /// neither yields a usable bundle the call fails instead of registering a
    /// lazy trust-directory lookup that would read files during a handshake.
    [[nodiscard]] static std::expected<std::shared_ptr<const TlsContext>, IoError> create(const TlsOptions& options);

    /// Borrowed OpenSSL handle; never null for a built context.
    [[nodiscard]] SSL_CTX* native_handle() const noexcept { return context_.get(); }

    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

private:
    /// Construction key: instances are created only by create().
    struct ConstructionKey {
        explicit ConstructionKey() = default;
    };

    /// Internal; use create().
    TlsContext(ConstructionKey, std::shared_ptr<SSL_CTX> context);

    std::shared_ptr<SSL_CTX> context_;
};

}  // namespace net

#endif  // YADDNSC_NET_TLS_CONTEXT_H
