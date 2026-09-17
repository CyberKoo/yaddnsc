//
// Public value types for the net::http client domain.
//

#ifndef YADDNSC_HTTP_TYPES_H
#define YADDNSC_HTTP_TYPES_H

#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "transport/options.h"

namespace net::http {

/// HTTP versions supported by the client.
enum class HttpVersion {
    V1_0,
    V1_1,
};

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
    std::multimap<std::string, std::string> headers{};
    std::optional<std::string> body{};
    std::string content_type{};

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
/// The body is stored as raw octets (binary-safe) and is only ever exposed
/// as views: text() for string payloads, bytes() for binary payloads.
class Response {
public:
    Response(int status_code, std::string body, std::multimap<std::string, std::string> response_headers,
             std::multimap<std::string, std::string> response_trailers = {})
        : status(status_code), headers(std::move(response_headers)), trailers(std::move(response_trailers)),
          body_(std::move(body)) {
    }

    int status;
    std::multimap<std::string, std::string> headers;
    /// Trailer fields received after a chunked response body.
    std::multimap<std::string, std::string> trailers;

    /// The body viewed as text (no encoding conversion is performed).
    [[nodiscard]] std::string_view text() const noexcept {
        return body_;
    }

    /// The body viewed as raw octets.
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept {
        return {reinterpret_cast<const std::uint8_t *>(body_.data()), body_.size()};
    }

    /// Body size in octets.
    [[nodiscard]] std::size_t size() const noexcept {
        return body_.size();
    }

private:
    std::string body_; ///< Owning octets.
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
    /// Protocol version emitted in requests. Responses may use either HTTP/1.0
    /// or HTTP/1.1.
    HttpVersion version{HttpVersion::V1_1};
    /// Allow the connection to be reused when the peer also permits it.
    bool keep_alive{true};
    Transport::Options transport{};
    Transport::TlsOptions tls{};
    std::string user_agent{};
    bool follow_redirects{true};
    int max_redirects{10};
    Limits limits{};
};

}  // namespace net::http

#endif  // YADDNSC_HTTP_TYPES_H
