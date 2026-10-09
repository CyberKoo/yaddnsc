//
// http — public value types, limits and options.
//

#ifndef YADDNSC_INFRASTRUCTURE_NET_HTTP_TYPES_H
#define YADDNSC_INFRASTRUCTURE_NET_HTTP_TYPES_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "domain/config/dns_config.h"
#include "domain/network/address_family.h"
#include "infrastructure/network/transport/options.h"
#include "infrastructure/network/transport/stream.h"

namespace http {

/// HTTP versions this client emits.
enum class HttpVersion {
    V1_0,
    V1_1,
};

/// Request methods this client emits.
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
/// The body is binary-safe. Header names/values are validated before
/// serialization; framing and routing headers (Host, Content-Length,
/// Content-Type, Connection, User-Agent) are owned by the client.
struct Request {
    Method method{Method::GET};
    std::multimap<std::string, std::string> headers{};
    std::optional<std::string> body{};
    std::string content_type{};

    /// Attach a text body (JSON, form-encoded, ...).
    void set_body(const std::string_view text) { body = std::string(text); }

    /// Attach a binary body (DNS wire format, serialized data, ...).
    void set_body(const std::span<const std::uint8_t> bytes) {
        body.emplace(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
};

/// An incoming HTTP response.
///
/// Ownership: owns its body; views returned by text()/bytes() are valid for the
/// lifetime of the Response. The body is stored as raw octets and exposed only
/// as views.
class Response {
public:
    Response(int status_code, std::string body, std::multimap<std::string, std::string> response_headers,
             std::multimap<std::string, std::string> response_trailers = {})
        : status_(status_code), headers_(std::move(response_headers)), trailers_(std::move(response_trailers)),
          body_(std::move(body)) {}

    int status_{0};
    std::multimap<std::string, std::string> headers_;
    /// Trailer fields received after a chunked body.
    std::multimap<std::string, std::string> trailers_;

    /// The body as text (no encoding conversion).
    [[nodiscard]] std::string_view text() const noexcept { return body_; }

    /// The body as raw octets.
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept {
        return {reinterpret_cast<const std::uint8_t*>(body_.data()), body_.size()};
    }

    /// Body size in octets.
    [[nodiscard]] std::size_t size() const noexcept { return body_.size(); }

private:
    std::string body_;
};

/// Size limits for a single exchange.
struct Limits {
    std::size_t max_header_bytes{64 * 1024};
    std::size_t max_body_bytes{16 * 1024 * 1024};
    /// Maximum number of informational (1xx) responses before the final one.
    std::size_t max_interim_responses{16};
};

/// Client-level options.
///
/// Carries the whole policy in one place: protocol preferences, transport and
/// TLS settings, and the bootstrap DNS servers used to turn a URL host into
/// addresses. There is no timeout anywhere — a deadline is the caller's cancel
/// scope.
struct Options {
    /// Version emitted in requests; responses may use either version.
    HttpVersion version{HttpVersion::V1_1};
    /// Allow connection reuse when the peer also permits it.
    bool keep_alive{true};
    /// User-Agent header value; empty emits no User-Agent header.
    std::string user_agent{};
    bool follow_redirects{true};
    int max_redirects{10};
    Limits limits{};
    /// Transport-level settings (outbound interface).
    net::ConnectOptions connect{};
    /// TLS settings (SNI, ALPN, verification, CA bundle).
    net::TlsOptions tls{};
    /// Pre-built TLS trust context, shared by every https stream this client
    /// opens. Null makes a TLS connection fail closed; build it off the loop
    /// with TlsContext::create before entering the loop.
    std::shared_ptr<const net::TlsContext> tls_context{};
    /// Bootstrap DNS servers (IP literals) used to resolve a URL host. Empty
    /// means hostnames fail fast with RESOLVE_FAILED; IP literals always work.
    /// /etc/hosts and NSS are never consulted.
    std::vector<domain::DnsServer> bootstrap_dns{};
    /// Restrict resolution to one address family.
    std::optional<domain::AddressFamily> address_family{};
    /// Stream source. Null selects the production factory; tests inject an
    /// in-memory factory here.
    std::shared_ptr<net::StreamFactory> factory{};
};

}  // namespace http

#endif  // YADDNSC_INFRASTRUCTURE_NET_HTTP_TYPES_H
