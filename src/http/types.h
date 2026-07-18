//
// Created by Kotarou on 2026/7/18.
//

#ifndef YADDNSC_HTTP_TYPES_H
#define YADDNSC_HTTP_TYPES_H

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace Http {

/// Parsed HTTP response headers.
struct ResponseHeaders {
    int status_code{};               ///< HTTP status code (e.g. 200, 404).
    size_t content_length{};         ///< Value of Content-Length (valid only when has_content_length).
    size_t header_end{};             ///< Offset of body start in the source buffer.
    bool has_content_length{false};  ///< Whether Content-Length was present.
    bool is_chunked{false};          ///< Whether Transfer-Encoding: chunked was present.
};

/// Complete HTTP response with binary body.
struct Response {
    int status_code{};
    std::vector<std::uint8_t> body{};
};

/// Errors during HTTP response parsing or body reading.
enum class Error {
    // ── Transport (propagated from Stream) ──
    TIMEOUT,            ///< I/O timed out.
    CANCELLED,          ///< Operation was externally cancelled.
    CONNECTION_FAILED,  ///< Connection lost or other fatal I/O error.

    // ── HTTP header parsing ──
    HEADER_PARSE_FAILED,    ///< Malformed HTTP response (picohttpparser returned -1).
    INCOMPLETE,             ///< More header data is needed (picohttpparser returned -2).
    CONTENT_TYPE_MISMATCH,  ///< Content-Type does not match expected value.
    HEADERS_TOO_LARGE,      ///< Header section exceeds maximum allowed size.

    // ── HTTP body reading ──
    BODY_TOO_LARGE,      ///< Body size exceeds maximum allowed size.
    CHUNK_PARSE_FAILED,  ///< Malformed chunked transfer encoding.
};

/// Return a human-readable name for an HTTP error code.
[[nodiscard]] constexpr std::string_view error_name(Error e) noexcept {
    using namespace std::string_view_literals;

    switch (e) {
        case Error::TIMEOUT:
            return "TIMEOUT"sv;
        case Error::CANCELLED:
            return "CANCELLED"sv;
        case Error::CONNECTION_FAILED:
            return "CONNECTION_FAILED"sv;
        case Error::HEADER_PARSE_FAILED:
            return "HEADER_PARSE_FAILED"sv;
        case Error::INCOMPLETE:
            return "INCOMPLETE"sv;
        case Error::CONTENT_TYPE_MISMATCH:
            return "CONTENT_TYPE_MISMATCH"sv;
        case Error::HEADERS_TOO_LARGE:
            return "HEADERS_TOO_LARGE"sv;
        case Error::BODY_TOO_LARGE:
            return "BODY_TOO_LARGE"sv;
        case Error::CHUNK_PARSE_FAILED:
            return "CHUNK_PARSE_FAILED"sv;
    }

    return "UNKNOWN"sv;
}

}  // namespace Http

#endif  // YADDNSC_HTTP_TYPES_H
