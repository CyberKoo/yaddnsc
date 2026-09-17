//
// Io error type for Transport.
//

#ifndef YADDNSC_NET_TRANSPORT_IO_ERROR_H
#define YADDNSC_NET_TRANSPORT_IO_ERROR_H

namespace Transport {

/// Errors that can occur during stream I/O or connection establishment.
enum class IoError {
    TIMEOUT,            ///< An operation timed out.
    CANCELLED,          ///< Cancellation was triggered; abort the operation.
    CONNECTION_FAILED,  ///< Non-recoverable connection or I/O error.
};

}  // namespace Transport

#endif  // YADDNSC_NET_TRANSPORT_IO_ERROR_H
