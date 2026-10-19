//
// net — error vocabulary for the coroutine transport layer.
//
// Recoverable transport failures use error values. Cancellation throws
// coro::Cancelled; deadlines are represented by the caller's ScopeOutcome.
//

#ifndef YADDNSC_INFRASTRUCTURE_NETWORK_TRANSPORT_IO_ERROR_H
#define YADDNSC_INFRASTRUCTURE_NETWORK_TRANSPORT_IO_ERROR_H

namespace net {

/// Failure vocabulary shared by every transport object.
enum class IoError {
    CONNECTION_FAILED,     ///< Unusable socket, connect/handshake failure, or EOF.
    TLS_HANDSHAKE_FAILED,  ///< The TLS session was never established: unusable
                           ///< trust material or session setup, or a handshake
                           ///< or certificate-verification failure. Distinct
                           ///< from CONNECTION_FAILED so the layer above can
                           ///< tell a peer that would not complete a
                           ///< negotiation from an unreachable socket.
};

}  // namespace net

#endif  // YADDNSC_INFRASTRUCTURE_NETWORK_TRANSPORT_IO_ERROR_H
