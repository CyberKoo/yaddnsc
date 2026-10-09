//
// net — TcpStream: a plain-TCP byte stream over the coroutine runtime.
//
// The coroutine transport: every operation is a lazy Task bound to the loop of
// the awaiting task, so a stream carries no reactor, no cancellation token and
// no timeout. A deadline is the caller's cancel scope; a cancelled await
// surfaces as coro::Cancelled.
//

#ifndef YADDNSC_INFRASTRUCTURE_NET_TRANSPORT_TCP_STREAM_H
#define YADDNSC_INFRASTRUCTURE_NET_TRANSPORT_TCP_STREAM_H

#include <cstddef>
#include <cstdint>
#include <span>

#include <expected>

#include "domain/network/inet_address.h"
#include "infrastructure/coro/task.hpp"
#include "infrastructure/network/transport/io_error.h"
#include "infrastructure/network/transport/options.h"
#include "infrastructure/network/transport/stream.h"
#include "support/util/fd.hpp"

namespace net {

/// A plain-TCP byte stream.
///
/// Targets are already-resolved addresses: hostname resolution needs the
/// bootstrap-DNS port, which arrives in stage 2b, so this stage accepts an IP
/// literal (or anything a resolver has produced) and nothing else.
///
/// Ownership: one stream owns one socket fd, released by close() or by
/// destruction. Non-movable: the address is stable for the lifetime of the
/// stream, which every Task the stream returns borrows.
/// Failure: every operation returns expected<T, IoError>. A failed connect
/// closes the socket and leaves the stream reusable; a peer close or any
/// unrecoverable socket error is CONNECTION_FAILED, including EOF on a read.
/// The transport never reports a timeout — a caller that wants one wraps the
/// call in `with_timeout(...)` and reads `ScopeOutcome::timed_out()`.
/// Thread safety: not thread-safe and not concurrent-safe. One stream serves one
/// operation at a time.
class TcpStream final : public Stream {
public:
    /// @param address  Destination address (IPv4 or IPv6 literal).
    /// @param port     Destination port, host byte order.
    /// @param options  Connection-level options.
    TcpStream(domain::InetAddress address, std::uint16_t port, ConnectOptions options = {});

    ~TcpStream();

    TcpStream(const TcpStream&) = delete;
    TcpStream& operator=(const TcpStream&) = delete;
    TcpStream(TcpStream&&) = delete;
    TcpStream& operator=(TcpStream&&) = delete;

    /// Establish the connection. Idempotent: a no-op while connected and
    /// healthy, otherwise the socket is (re)built. Cancellable at the connect.
    [[nodiscard]] coro::Task<std::expected<void, IoError>> ensure_connected() override;

    /// Read at least one byte, up to `buf.size()`. EOF is CONNECTION_FAILED; an
    /// empty buffer performs no I/O and succeeds with 0.
    [[nodiscard]] coro::Task<std::expected<std::size_t, IoError>> read_some(std::span<std::uint8_t> buf) override;

    /// Read exactly `buf.size()` bytes. A short read (EOF mid-message) fails.
    [[nodiscard]] coro::Task<std::expected<void, IoError>> read_exact(std::span<std::uint8_t> buf) override;

    /// Write every byte of `data`.
    [[nodiscard]] coro::Task<std::expected<void, IoError>> send_all(std::span<const std::uint8_t> data) override;

    /// Release the socket. Idempotent; never throws.
    void close() noexcept override;

    /// True while a connection is believed good (a one-syscall peek; a peer
    /// close is reported on the next read regardless).
    [[nodiscard]] bool connected() const noexcept override;

    /// Borrowed descriptor, for layering (TLS attaches it to an SSL session).
    /// -1 when closed. Valid until close() or destruction.
    [[nodiscard]] int native_handle() const noexcept { return fd_.get(); }

    /// Destination address. Borrowed; lives as long as the stream.
    [[nodiscard]] const domain::InetAddress& address() const noexcept { return address_; }

    /// Destination port, host byte order.
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    /// One non-blocking peek: data pending or an open peer counts as healthy,
    /// EOF does not. Never blocks.
    [[nodiscard]] bool is_healthy() const noexcept;

    domain::InetAddress address_;
    std::uint16_t port_ = 0;
    ConnectOptions options_;
    Utils::UniqueFd fd_;
    bool connected_ = false;
};

}  // namespace net

#endif  // YADDNSC_INFRASTRUCTURE_NET_TRANSPORT_TCP_STREAM_H
