//
// http — public value types, limits and options.
//

#ifndef YADDNSC_INFRASTRUCTURE_HTTP_TYPES_H
#define YADDNSC_INFRASTRUCTURE_HTTP_TYPES_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "domain/network/address_family.h"
#include "infrastructure/network/address/host_resolver.h"
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
    DELETE,
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
    /// Explicitly safe to replay on a lost reused connection (e.g. a read-only
    /// POST). Default false; callers must establish idempotence before enabling.
    bool replay_safe{false};

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

/// What one exchange observed about the connection its response arrived on.
///
/// These are wire facts, not a reuse decision: whether the connection carries
/// another request also depends on the local request policy and on the
/// keep-alive budget, which the session layer owns.
struct ResponseFraming {
    /// The version the response was framed with.
    HttpVersion version{HttpVersion::V1_1};
    /// The body ended at a boundary the peer stated (a length, chunked coding,
    /// or no body at all), so the next response on this connection starts where
    /// this one ended.
    bool self_delimiting{false};
    /// The peer announced that it closes the connection after this response.
    bool connection_close{false};
    /// The peer explicitly permits reuse (HTTP/1.0 `Connection: keep-alive`).
    bool keep_alive{false};
    /// Request cap the peer states for this connection (`Keep-Alive: max=`).
    std::optional<unsigned> max;
    /// Idle window the peer grants this connection (`Keep-Alive: timeout=`).
    std::optional<unsigned> timeout;
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
/// TLS settings, and the injected resolver used to turn a URL host into
/// addresses. The caller scope bounds the exchange; optional local queue and
/// per-address connect budgets can fail a busy session or skip a black-holed
/// candidate without exhausting that entire exchange budget.
struct Options {
    /// Version emitted in requests; responses may use either version.
    HttpVersion version{HttpVersion::V1_1};
    /// Allow connection reuse when the peer also permits it.
    bool keep_alive{true};
    /// User-Agent header value; empty emits no User-Agent header.
    std::string user_agent{};
    bool follow_redirects{true};
    /// Total follow-up hops per logical exchange, including cross-origin handoff.
    int max_redirects{10};
    Limits limits{};
    /// Per-address connection budget, including the TLS handshake. An expired
    /// budget skips to the next resolved address; zero disables it. This is not
    /// a whole-exchange deadline: response reads require the caller's scope.
    std::chrono::milliseconds connect_timeout{5000};
    /// Optional session-queue budget; zero leaves queueing to the caller scope.
    std::chrono::milliseconds queue_timeout{0};
    /// Transport-level settings (outbound interface).
    net::ConnectOptions connect{};
    /// TLS settings (SNI, ALPN). Trust policy comes from the context below.
    net::TlsOptions tls{};
    /// Pre-built TLS trust context, shared by every https stream this client
    /// opens. Null makes a TLS connection fail closed; build it off the loop
    /// with TlsContext::create before entering the loop.
    std::shared_ptr<const net::TlsContext> tls_context{};
    /// Hostname resolver supplied by the assembly layer. Empty means hostnames
    /// fail fast with RESOLVE_FAILED; IP literals bypass the resolver.
    net::HostResolver resolve{};
    /// Restrict resolution to one address family.
    std::optional<domain::AddressFamily> address_family{};
    /// Stream source. Null selects the production factory; tests inject an
    /// in-memory factory here.
    std::shared_ptr<net::StreamFactory> factory{};
};

}  // namespace http

#endif  // YADDNSC_INFRASTRUCTURE_HTTP_TYPES_H
