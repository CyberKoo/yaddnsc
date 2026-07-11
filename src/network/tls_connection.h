//
// Created by Kotarou on 2026/7/10.
//

#ifndef YADDNSC_NETWORK_TLS_CONNECTION_H
#define YADDNSC_NETWORK_TLS_CONNECTION_H

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <openssl/bio.h>
#include <openssl/ssl.h>

// ── RAII deleters for OpenSSL resources (implemented in .cpp) ──

struct SSLContextDeleter {
    void operator()(SSL_CTX *ctx) const noexcept;
};

struct BIODeleter {
    void operator()(BIO *bio) const noexcept;
};

using SslCtxPtr = std::unique_ptr<SSL_CTX, SSLContextDeleter>;
using BioPtr = std::unique_ptr<BIO, BIODeleter>;

// ── TlsConnection ──

/// A stateful TLS connection over OpenSSL BIO.
///
/// Manages the full lifecycle: connect, I/O (with poll-based timeouts and
/// optional cancellation fd), health check, close, and reconnect.
///
/// By default the underlying SSL_CTX is shared across all instances via a
/// lazy-initialised function-local static.  A custom @c ContextFactory can be
/// passed to the constructor to override this — useful for custom certificate
/// verification, client certificates, or other per-connection SSL configuration.
///
/// Thread safety: distinct `TlsConnection` objects are independent. A single
/// object is **not** thread-safe; external synchronisation is required if
/// shared across threads.
class TlsConnection {
public:
    /// Result of an I/O operation.
    enum class IoStatus {
        OK, ///< Operation completed successfully.
        TIMEOUT, ///< poll() timed out without completing the I/O.
        ERROR, ///< Non-recoverable I/O error (connection lost, SSL fatal, etc.).
        CANCELLED ///< Cancel fd was signalled (caller should abort).
    };

    /// Factory for creating a custom SSL_CTX.
    ///
    /// The returned context is cached inside the connection and reused on
    /// reconnect.  If no factory is provided, a default client context is
    /// lazily created once and shared by all `TlsConnection` instances (see
    /// the class-level documentation).
    using ContextFactory = std::function<SslCtxPtr()>;

    /// Construct a TLS connection (not yet connected).
    ///
    /// @param server           Server hostname or IP address.
    /// @param port             TCP port.
    /// @param connect_timeout  Maximum time to wait for the TLS handshake.
    /// @param sni_hostname     Optional SNI hostname override. When set, it is
    ///                         used for both the TLS SNI extension and certificate
    ///                         hostname verification.  When nullopt (default),
    ///                         the connection target (@p server) is used for both.
    /// @param alpn_proto       Optional ALPN protocol bytes (e.g. {3, 'd', 'o', 't'}).
    /// @param context_factory  Optional factory for a custom SSL_CTX.
    ///                         When null (default), the shared default context
    ///                         is used.
    /// @throws TlsException on invalid server address.
    TlsConnection(std::string server, std::uint16_t port, std::chrono::milliseconds connect_timeout,
                  std::optional<std::string> sni_hostname = std::nullopt,
                  std::span<const unsigned char> alpn_proto = {}, ContextFactory context_factory = {});

    ~TlsConnection();

    // Move-only.
    TlsConnection(TlsConnection &&) noexcept;

    TlsConnection &operator=(TlsConnection &&) noexcept;

    TlsConnection(const TlsConnection &) = delete;

    TlsConnection &operator=(const TlsConnection &) = delete;

    // ── Lifecycle ──

    /// Open (or re-establish) the TLS connection.
    ///
    /// If already connected, the old connection is closed first.
    /// @return  std::expected<void, IoStatus> — empty on success, error on failure.
    [[nodiscard]] std::expected<void, IoStatus> connect();

    /// Close the connection.
    void close() noexcept;

    /// Whether the underlying BIO is currently valid.
    [[nodiscard]] bool is_connected() const noexcept { return bio_ != nullptr; }

    /// Quick health check — returns false if the peer has closed the connection.
    /// Correctly handles pending application data (does not treat buffered
    /// readable data as a closed connection).
    [[nodiscard]] bool is_healthy() const noexcept;

    /// Set the timeout for I/O operations (send/read/shutdown).
    /// The default is 5 seconds.  Pass 0ms for non-blocking behaviour
    /// (returns immediately on all I/O calls).
    void set_io_timeout(std::chrono::milliseconds timeout) noexcept { io_timeout_ms_ = timeout; }

    // ── I/O ──

    /// Send all bytes in `data`.
    ///
    /// @param data      Bytes to send.
    /// @param cancel_fd Optional file descriptor for cancellation.  When it
    ///                  becomes readable, the operation is aborted and
    ///                  @c IoStatus::CANCELLED is returned.
    /// @return          std::expected<void, IoStatus> — empty on success, error code on failure.
    [[nodiscard]] std::expected<void, IoStatus> send_all(std::span<const std::uint8_t> data, int cancel_fd = -1);

    /// Read exactly `buf.size()` bytes.
    ///
    /// @param buf       Buffer to fill.
    /// @param cancel_fd Optional file descriptor for cancellation.  When it
    ///                  becomes readable, the operation is aborted and
    ///                  @c IoStatus::CANCELLED is returned.
    /// @return          std::expected<void, IoStatus> — empty on success, error code on failure.
    [[nodiscard]] std::expected<void, IoStatus> read_exact(std::span<std::uint8_t> buf, int cancel_fd = -1);

    /// Read at least one byte (partial read).
    ///
    /// Returns the number of bytes actually read, which may be less than
    /// `buf.size()`.  This is useful for reading an HTTP response where
    /// the total length is not yet known.
    ///
    /// @param buf       Buffer to fill.
    /// @param cancel_fd Optional file descriptor for cancellation.
    /// @return          Bytes read on success, or an error code.
    [[nodiscard]] std::expected<size_t, IoStatus> read_some(std::span<std::uint8_t> buf, int cancel_fd = -1);

    // ── TLS protocol helpers ──

    /// The ALPN protocol negotiated during the TLS handshake.
    /// Returns an empty string if no ALPN was negotiated.
    [[nodiscard]] std::string negotiated_alpn() const noexcept;

    /// Send a TLS close_notify alert (half-close the write direction).
    ///
    /// After a successful return, sending data is no longer allowed but
    /// reading may continue (the peer may still send a response before
    /// its own close_notify).  Callers should finish reading the response
    /// and then call @c close().
    /// @return  std::expected<void, IoStatus> — empty on success, error code on failure.
    [[nodiscard]] std::expected<void, IoStatus> shutdown();

    // ── SNI / certificate hostname ──

    /// Override the hostname used for both TLS SNI and certificate
    /// verification.
    ///
    /// By default the connection target (@p server passed to the
    /// constructor) is used for both purposes: it is sent as SNI and
    /// verified against the peer certificate.  Call this before
    /// @c connect() to override both with a different hostname.
    void set_sni_hostname(std::string hostname);

    // ── Raw access ──

    /// Direct access to the underlying BIO (for logging, debugging, etc.).
    [[nodiscard]] BIO *native_handle() const noexcept { return bio_.get(); }

    /// Direct access to the underlying SSL object.
    [[nodiscard]] SSL *native_ssl() const noexcept { return ssl_; }

private:
    enum class PollEvent { READ, WRITE };

    [[nodiscard]] IoStatus poll_bio(BIO *bio, PollEvent event, int cancel_fd);

    [[nodiscard]] static SslCtxPtr create_default_ssl_ctx();

    [[nodiscard]] static SSL_CTX *get_shared_ssl_ctx();

    std::string server_;
    std::uint16_t port_;
    std::chrono::milliseconds connect_timeout_;
    std::optional<std::string> sni_hostname_;
    std::vector<unsigned char> alpn_proto_;
    ContextFactory context_factory_;
    std::chrono::milliseconds io_timeout_ms_{5000}; ///< I/O poll timeout.
    SslCtxPtr custom_ctx_; ///< Cached result of context_factory_ (can be null).
    BioPtr bio_;
    SSL *ssl_ = nullptr; ///< Non-owning pointer into bio_; null when disconnected.
};

#endif  // YADDNSC_NETWORK_TLS_CONNECTION_H
