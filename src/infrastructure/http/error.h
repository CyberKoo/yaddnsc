//
// http — error model for the coroutine HTTP client.
//
// Recoverable transport failures use error values. Cancellation throws
// coro::Cancelled; deadlines are represented by the caller's ScopeOutcome.
//

#ifndef YADDNSC_INFRASTRUCTURE_HTTP_ERROR_H
#define YADDNSC_INFRASTRUCTURE_HTTP_ERROR_H

#include <cstdint>
#include <string>

namespace http {

/// Machine-readable error classification for HTTP operations.
enum class ErrorCode {

    RESOLVE_FAILED,           ///< The host could not be resolved.
    CONNECT_FAILED,           ///< TCP connect failed for every candidate address.
    TLS_HANDSHAKE_FAILED,     ///< TLS handshake or certificate verification failed.
    CONNECTION_LOST,          ///< I/O failed on an established connection.
    RESPONSE_PARSE_FAILED,    ///< Malformed response (headers, framing, or chunk coding).
    HEADERS_TOO_LARGE,        ///< The header section across the limit.
    BODY_TOO_LARGE,           ///< The body across the limit.
    REDIRECT_LIMIT_EXCEEDED,  ///< Too many redirects.
    INVALID_URL,              ///< The request URL could not be parsed.
    INVALID_REQUEST,          ///< Unsafe or malformed request target/header.
    UNSUPPORTED_PROTOCOL,     ///< Protocol upgrade or unsupported HTTP feature.
};

/// Error value: a stable code plus a human-readable message.
struct Error {
    ErrorCode code{ErrorCode::CONNECTION_LOST};
    std::string message;
    /// Rate-limit hint carried through to the plugin ABI (0 = no hint).
    uint32_t retry_after_seconds{0};
};

}  // namespace http

#endif  // YADDNSC_INFRASTRUCTURE_HTTP_ERROR_H
