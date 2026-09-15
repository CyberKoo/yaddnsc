//
// Error model for the net::http client domain.
//
// A structured error (code + message) is used across the domain; strings
// alone are not an error protocol.
//

#ifndef YADDNSC_HTTP_CLIENT_ERROR_H
#define YADDNSC_HTTP_CLIENT_ERROR_H

#include <string>

namespace net::http {

/// Machine-readable error classification for HTTP operations.
enum class ErrorCode {
    CANCELLED,                ///< Operation aborted via the cancellation token.
    TIMEOUT,                  ///< An operation timed out.
    RESOLVE_FAILED,           ///< Name resolution failed.
    CONNECT_FAILED,           ///< TCP connect failed (all addresses exhausted).
    TLS_HANDSHAKE_FAILED,     ///< TLS handshake or verification failed.
    CONNECTION_LOST,          ///< I/O failed on an established connection.
    RESPONSE_PARSE_FAILED,    ///< Malformed response (headers or chunk framing).
    HEADERS_TOO_LARGE,        ///< Response header section exceeds the limit.
    BODY_TOO_LARGE,           ///< Response body exceeds the limit.
    REDIRECT_LIMIT_EXCEEDED,  ///< Too many redirects or a redirect loop.
    INVALID_URL,              ///< The request URL could not be parsed.
    INVALID_REQUEST,          ///< Unsafe or malformed request target/header.
    UNSUPPORTED_PROTOCOL,     ///< A protocol upgrade or unsupported HTTP feature.
};

/// Error value: a stable code plus a human-readable message.
struct Error {
    ErrorCode code;
    std::string message;
};

}  // namespace net::http

#endif  // YADDNSC_HTTP_CLIENT_ERROR_H
