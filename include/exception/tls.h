//
// Created by Kotarou on 2026/7/10.
//

#ifndef YADDNSC_EXCEPTION_TLS_H
#define YADDNSC_EXCEPTION_TLS_H

#include <stdexcept>
#include <string_view>

/// Thrown when a TLS connection operation fails.
///
/// Used by the TLS connection module (network/tls_connection.h) to signal
/// TLS-specific failures (connection errors, handshake failures, timeouts,
/// certificate verification issues, etc.) without leaking DNS-layer types.
class TlsException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

#endif  // YADDNSC_EXCEPTION_TLS_H
