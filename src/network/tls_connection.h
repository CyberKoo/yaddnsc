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

// ── Forward declarations ──

namespace Utils {
class CancellationToken;
}

// ── RAII deleters for OpenSSL resources (implemented in .cpp) ──

struct SSLContextDeleter {
    void operator()(SSL_CTX *ctx) const noexcept;
};

struct BIODeleter {
    void operator()(BIO *bio) const noexcept;
};

using SslCtxPtr = std::unique_ptr<SSL_CTX, SSLContextDeleter>;
using BioPtr = std::unique_ptr<BIO, BIODeleter>;

// ── Optional TLS connection parameters ──

/// Aggregated optional configuration for @ref TlsConnection.
///
/// A simple aggregate so callers can use designated initialisers:
/// @code
///   TlsOptions{.alpn_proto = ALPN_HTTP}
/// @endcode
///
/// All fields have sensible defaults; only set the ones you need.
struct TlsOptions {
    /// SNI / certificate hostname override (default: use @p server from constructor).
    std::optional<std::string> sni_hostname{std::nullopt};

    /// ALPN protocol bytes (e.g. @c {2, 'h','2'}).
    std::span<const unsigned char> alpn_proto{};

    /// Maximum time to wait for the TLS handshake.
    std::chrono::milliseconds connect_timeout{1500};

    /// Timeout for each individual @c poll() call during reads (including
    /// @c shutdown).  Pass @c 0ms for fully non-blocking behaviour.
    std::chrono::milliseconds read_timeout{1500};

    /// Timeout for each individual @c poll() call during writes.
    std::chrono::milliseconds write_timeout{1500};
};

// ── TlsConnectionBase ──

/// Abstract base class for TLS connections.
///
/// Defines the interface used by all TLS-dependent components
/// (DohResolver, DotResolver, TlsStream).  The concrete @ref TlsConnection
/// implements this over OpenSSL BIO.
///
/// @note The @c IoStatus enum is defined here and inherited by
///       @ref TlsConnection, so all existing references to
///       @c TlsConnection::IoStatus remain valid.
///
/// Thread safety: distinct objects are independent. A single object is
/// **not** thread-safe; external synchronisation is required if shared
/// across threads.
class TlsConnectionBase {
public:
    /// Result of an I/O operation.
    enum class IoStatus {
        OK,        ///< Operation completed successfully.
        TIMEOUT,   ///< poll() timed out without completing the I/O.
        ERROR,     ///< Non-recoverable I/O error (connection lost, SSL fatal, etc.).
        CANCELLED  ///< Cancel fd was signalled (caller should abort).
    };

    virtual ~TlsConnectionBase() = default;

    TlsConnectionBase() = default;
    TlsConnectionBase(TlsConnectionBase &&) noexcept = default;
    TlsConnectionBase &operator=(TlsConnectionBase &&) noexcept = default;
    TlsConnectionBase(const TlsConnectionBase &) = delete;
    TlsConnectionBase &operator=(const TlsConnectionBase &) = delete;

    // ── Lifecycle ──

    /// Open (or re-establish) the TLS connection.
    /// @return  std::expected<void, IoStatus> — empty on success, error on failure.
    [[nodiscard]] virtual std::expected<void, IoStatus> connect() = 0;

    /// Close the connection.
    virtual void close() noexcept = 0;

    /// Whether the connection has been established.
    [[nodiscard]] virtual bool is_connected() const noexcept = 0;

    /// Quick health check — returns false if the peer has closed the connection.
    [[nodiscard]] virtual bool is_healthy() const noexcept = 0;

    // ── I/O (all variants accept an optional cancellation token) ──

    /// Send all bytes in @p data.
    [[nodiscard]] virtual std::expected<void, IoStatus> send_all(
        std::span<const std::uint8_t> data,
        const Utils::CancellationToken &cancel_token) = 0;

    /// Read exactly @p buf.size() bytes.
    [[nodiscard]] virtual std::expected<void, IoStatus> read_exact(
        std::span<std::uint8_t> buf,
        const Utils::CancellationToken &cancel_token) = 0;

    /// Read at least one byte (partial read).
    [[nodiscard]] virtual std::expected<size_t, IoStatus> read_some(
        std::span<std::uint8_t> buf,
        const Utils::CancellationToken &cancel_token) = 0;

    // ── TLS protocol helpers ──

    /// Send a TLS close_notify alert (half-close the write direction).
    [[nodiscard]] virtual std::expected<void, IoStatus> shutdown() = 0;

    /// The ALPN protocol negotiated during the TLS handshake.
    /// Returns an empty string if no ALPN was negotiated.
    [[nodiscard]] virtual std::string negotiated_alpn() const noexcept = 0;

    /// Override the hostname used for both TLS SNI and certificate verification.
    virtual void set_sni_hostname(std::string hostname) = 0;
};

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
class TlsConnection : public TlsConnectionBase {
public:
    /// Factory for creating a custom SSL_CTX.
    ///
    /// The returned context is cached inside the connection and reused on
    /// reconnect.  If no factory is provided, a default client context is
    /// lazily created once and shared by all `TlsConnection` instances (see
    /// the class-level documentation).
    using ContextFactory = std::function<SslCtxPtr()>;

    /// Construct a TLS connection.
    ///
    /// @param server           Server hostname or IP address.
    /// @param port             TCP port.
    /// @param opts             Optional configuration (SNI, ALPN, timeouts).
    /// @param context_factory  Optional factory for a custom SSL_CTX.
    ///                         When null (default), the shared default context
    ///                         is used.
    /// @throws TlsException on invalid server address.
    TlsConnection(std::string server, std::uint16_t port, TlsOptions opts = {}, ContextFactory context_factory = {});

    ~TlsConnection() override;

    TlsConnection(TlsConnection &&) noexcept = default;

    TlsConnection &operator=(TlsConnection &&) noexcept = default;

    TlsConnection(const TlsConnection &) = delete;

    TlsConnection &operator=(const TlsConnection &) = delete;

    // ── Lifecycle ──

    /// Open (or re-establish) the TLS connection.
    ///
    /// If already connected, the old connection is closed first.
    /// @return  std::expected<void, IoStatus> — empty on success, error on failure.
    [[nodiscard]] std::expected<void, IoStatus> connect() override;

    /// Close the connection.
    void close() noexcept override;

    /// Whether the underlying BIO is currently valid.
    [[nodiscard]] bool is_connected() const noexcept override { return bio_ != nullptr; }

    /// Quick health check — returns false if the peer has closed the connection.
    /// Correctly handles pending application data (does not treat buffered
    /// readable data as a closed connection).
    [[nodiscard]] bool is_healthy() const noexcept override;

    /// Set the timeout for read operations (including shutdown).
    /// The default is 5 seconds.  Pass 0ms for fully non-blocking behaviour.
    void set_read_timeout(std::chrono::milliseconds timeout) noexcept { read_timeout_ms_ = timeout; }

    /// Set the timeout for write operations.
    /// The default is 5 seconds.  Pass 0ms for fully non-blocking behaviour.
    void set_write_timeout(std::chrono::milliseconds timeout) noexcept { write_timeout_ms_ = timeout; }

    // ── I/O ──

    /// Send all bytes in `data` (no cancellation support).
    [[nodiscard]] std::expected<void, IoStatus> send_all(std::span<const std::uint8_t> data);

    /// Send all bytes in `data` with optional cancellation support.
    /// @param cancel_token  When active, the operation is aborted and
    ///                      @c IoStatus::CANCELLED is returned on trigger.
    [[nodiscard]] std::expected<void, IoStatus> send_all(std::span<const std::uint8_t> data,
                                                         const Utils::CancellationToken &cancel_token) override;

    /// Read exactly `buf.size()` bytes (no cancellation support).
    [[nodiscard]] std::expected<void, IoStatus> read_exact(std::span<std::uint8_t> buf);

    /// Read exactly `buf.size()` bytes with optional cancellation support.
    /// @param cancel_token  When active, the operation is aborted and
    ///                      @c IoStatus::CANCELLED is returned on trigger.
    [[nodiscard]] std::expected<void, IoStatus> read_exact(std::span<std::uint8_t> buf,
                                                           const Utils::CancellationToken &cancel_token) override;

    /// Read at least one byte (partial read, no cancellation support).
    /// Returns the number of bytes actually read, which may be less than
    /// `buf.size()`.  Useful for reading an HTTP response where the total
    /// length is not yet known.
    [[nodiscard]] std::expected<size_t, IoStatus> read_some(std::span<std::uint8_t> buf);

    /// Read at least one byte with optional cancellation support.
    [[nodiscard]] std::expected<size_t, IoStatus> read_some(std::span<std::uint8_t> buf,
                                                            const Utils::CancellationToken &cancel_token) override;

    // ── TLS protocol helpers ──

    /// The ALPN protocol negotiated during the TLS handshake.
    /// Returns an empty string if no ALPN was negotiated.
    [[nodiscard]] std::string negotiated_alpn() const noexcept override;

    /// Send a TLS close_notify alert (half-close the write direction).
    ///
    /// After a successful return, sending data is no longer allowed but
    /// reading may continue (the peer may still send a response before
    /// its own close_notify).  Callers should finish reading the response
    /// and then call @c close().
    /// @return  std::expected<void, IoStatus> — empty on success, error code on failure.
    [[nodiscard]] std::expected<void, IoStatus> shutdown() override;

    // ── SNI / certificate hostname ──

    /// Override the hostname used for both TLS SNI and certificate
    /// verification.
    ///
    /// By default the connection target (@p server passed to the
    /// constructor) is used for both purposes: it is sent as SNI and
    /// verified against the peer certificate.  Call this before
    /// @c connect() to override both with a different hostname.
    void set_sni_hostname(std::string hostname) override;

    // ── Raw access ──

    /// Direct access to the underlying BIO (for logging, debugging, etc.).
    [[nodiscard]] BIO *native_handle() const noexcept { return bio_.get(); }

    /// Direct access to the underlying SSL object (obtained from the BIO on demand).
    [[nodiscard]] SSL *native_ssl() const noexcept {
        SSL *ssl = nullptr;
        if (bio_)
            BIO_get_ssl(bio_.get(), &ssl);
        return ssl;
    }

private:
    [[nodiscard]] IoStatus poll_bio(BIO *bio, short default_events, const Utils::CancellationToken &cancel_token,
                                    std::chrono::milliseconds timeout);

    [[nodiscard]] static SslCtxPtr create_default_ssl_ctx();

    [[nodiscard]] static SSL_CTX *get_shared_ssl_ctx();

    std::string server_;
    std::uint16_t port_;
    std::optional<std::string> sni_hostname_;
    std::chrono::milliseconds connect_timeout_;
    std::chrono::milliseconds read_timeout_ms_;
    std::chrono::milliseconds write_timeout_ms_;
    std::vector<unsigned char> alpn_proto_;
    ContextFactory context_factory_;

    SslCtxPtr custom_ctx_; ///< Cached result of context_factory_ (can be null).
    BioPtr bio_;

    [[nodiscard]] SSL *get_ssl() const noexcept;
};

#endif  // YADDNSC_NETWORK_TLS_CONNECTION_H
