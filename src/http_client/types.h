//
// Public value types for the net::http client domain.
//

#ifndef YADDNSC_HTTP_CLIENT_TYPES_H
#define YADDNSC_HTTP_CLIENT_TYPES_H

#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "network/transport/options.h"

namespace net::http {

/// HTTP request methods supported by the client.
enum class Method {
    GET,
    POST,
    PUT,
    DEL,
    PATCH,
    HEAD,
    OPTIONS,
};

/// An outgoing HTTP request.
///
/// The body is binary-safe: it can carry arbitrary bytes (including NULs).
/// Use set_body() to attach text or binary payloads explicitly.
struct Request {
    Method method;
    std::multimap<std::string, std::string> headers;
    std::optional<std::string> body;
    std::string content_type;

    /// Attach a text body (e.g. JSON, form-encoded data).
    void set_body(const std::string_view text) {
        body = std::string(text);
    }

    /// Attach a binary body (e.g. DNS wire format, serialized data).
    void set_body(const std::span<const std::uint8_t> bytes) {
        body.emplace(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    }
};

/// An incoming HTTP response.
///
/// The body is binary-safe raw bytes. Access it as text via body_text() or
/// as bytes via body_bytes(), depending on what the caller expects.
struct Response {
    int status;
    std::string body; ///< Raw body bytes (binary-safe).
    std::multimap<std::string, std::string> headers;

    /// The body viewed as text (no encoding conversion is performed).
    [[nodiscard]] std::string_view body_text() const noexcept {
        return body;
    }

    /// The body viewed as raw bytes.
    [[nodiscard]] std::span<const std::uint8_t> body_bytes() const noexcept {
        return {reinterpret_cast<const std::uint8_t *>(body.data()), body.size()};
    }
};

/// Size limits for a single exchange.
struct Limits {
    size_t max_header_bytes = 64 * 1024;
    size_t max_body_bytes = 16 * 1024 * 1024;
};

/// Client-level options.
///
/// Embeds the transport options so everything is configured once, at
/// construction.
struct Options {
    Transport::Options transport;
    Transport::TlsOptions tls;
    std::string user_agent;
    bool follow_redirects{true};
    int max_redirects{10};
    Limits limits;
};

}  // namespace net::http

#endif  // YADDNSC_HTTP_CLIENT_TYPES_H
