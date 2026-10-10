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
    CONNECTION_FAILED,  ///< Unusable socket, connect/handshake failure, or EOF.
};

}  // namespace net

#endif  // YADDNSC_INFRASTRUCTURE_NETWORK_TRANSPORT_IO_ERROR_H
