//
// http — error model for the coroutine HTTP client.
//
// A structured error (code + message) crosses the domain; strings alone are not
// an error protocol. There is no TIMEOUT code: a deadline belongs to the
// caller's cancel scope, which aborts a pending await with CANCELLED and reports
// the reason itself through ScopeOutcome::timed_out().
//

#ifndef YADDNSC_NET_HTTP_ERROR_H
#define YADDNSC_NET_HTTP_ERROR_H

#include <string>

namespace http {

/// Machine-readable error classification for HTTP operations.
enum class ErrorCode {
    CANCELLED,                ///< The enclosing cancel scope aborted the operation.
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
};

}  // namespace http

#endif  // YADDNSC_HTTP_ERROR_H
