//
// http — name resolution and stream connection shared by the clients.
//

#include "transport.h"

#include <coroutine>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include <expected>
#include <string>
#include <utility>
#include <optional>

#include "infrastructure/http/wire_request.h"
#include "infrastructure/network/factory/default_stream_factory.h"
#include "domain/error/dns_error_info.h"
#include "infrastructure/http/types.h"
#include "infrastructure/network/transport/options.h"

namespace http {

coro::Task<std::expected<std::vector<domain::InetAddress>, Error>> resolve_host(std::string host,
                                                                                const Options& options) {
    if (const auto literal = domain::InetAddress::parse(host)) {
        co_return std::vector<domain::InetAddress>{*literal};
    }
    if (!options.resolve) {
        co_return std::unexpected(Error{ErrorCode::RESOLVE_FAILED, "no hostname resolver configured"});
    }
    auto resolved = co_await options.resolve(std::move(host), options.address_family);
    if (!resolved) {
        co_return std::unexpected(Error{ErrorCode::RESOLVE_FAILED, resolved.error().message});
    }
    co_return std::move(*resolved);
}

coro::Task<std::expected<std::unique_ptr<net::Stream>, Error>> connect_stream(
    const std::string_view scheme, const std::string_view host, const std::span<const domain::InetAddress> addresses,
    const std::uint16_t port, const Options& options) {
    const bool tls = scheme == "https";

    // A stateless fallback keeps the production factory out of global state while
    // leaving the injection point (Options::factory) authoritative.
    net::DefaultStreamFactory fallback;
    net::StreamFactory& factory = options.factory != nullptr ? *options.factory : fallback;

    // A host*name* is the default TLS identity (SNI and certificate
    // verification); a name pinned in Options::tls wins. An IP-literal host is
    // never copied: RFC 6066 §3 forbids an IP literal in SNI, and verification
    // then targets the connection IP by default.
    net::TlsOptions tls_options = options.tls;
    if (tls && !tls_options.sni_hostname.has_value() && !domain::InetAddress::parse(host).has_value()) {
        tls_options.sni_hostname = std::string(host);
    }

    Error last{ErrorCode::CONNECT_FAILED, "no address to connect to"};
    for (const domain::InetAddress& address : addresses) {
        auto stream = tls ? factory.create_tls(address, port, options.connect, tls_options, options.tls_context)
                          : factory.create_tcp(address, port, options.connect);
        auto connected = co_await stream->ensure_connected();
        if (connected) {
            co_return stream;
        }
        last = connect_error();
    }
    co_return std::unexpected(std::move(last));
}

}  // namespace http
