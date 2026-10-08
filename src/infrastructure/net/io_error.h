//
// net — error vocabulary for the coroutine transport layer.
//
// Cancellation is a value on this channel, not an exception and not a timeout:
// the transport never times out on its own. A deadline belongs to the caller's
// cancel scope, which aborts the pending await with CANCELLED and reports the
// reason itself (`ScopeOutcome::timed_out()`).
//

#ifndef YADDNSC_NET_IO_ERROR_H
#define YADDNSC_NET_IO_ERROR_H

namespace net {

/// Failure vocabulary shared by every transport object.
enum class IoError {
    CANCELLED,          ///< The enclosing cancel scope aborted the operation.
    CONNECTION_FAILED,  ///< Unusable socket, connect/handshake failure, or EOF.
};

}  // namespace net

#endif  // YADDNSC_NET_IO_ERROR_H
