//
// http — name resolution and stream connection shared by the clients.
//

#include "transport.h"

#include <utility>

#include "infrastructure/dns/bootstrap.h"
#include "infrastructure/net/http/wire_request.h"
#include "support/fmt.hpp"

namespace http {

coro::Task<std::expected<std::vector<InetAddress>, Error>> resolve_host(std::string host, const Options& options) {
    if (const auto literal = InetAddress::parse(host)) {
        co_return std::vector<InetAddress>{*literal};
    }
    auto resolved = co_await dns::bootstrap_resolve(std::move(host), options.address_family, options.bootstrap_dns);
    if (!resolved) {
        co_return std::unexpected(Error{ErrorCode::RESOLVE_FAILED, resolved.error().message});
    }
    co_return std::move(*resolved);
}

coro::Task<std::expected<std::unique_ptr<net::Stream>, Error>> connect_stream(
    const std::string_view scheme, const std::span<const InetAddress> addresses, const std::uint16_t port,
    const Options& options) {
    const bool tls = scheme == "https";

    // A stateless fallback keeps the production factory out of global state while
    // leaving the injection point (Options::factory) authoritative.
    net::DefaultStreamFactory fallback;
    net::StreamFactory& factory = options.factory != nullptr ? *options.factory : fallback;

    Error last{ErrorCode::CONNECT_FAILED, "no address to connect to"};
    for (const InetAddress& address : addresses) {
        auto stream = tls ? factory.create_tls(address, port, options.connect, options.tls, options.tls_context)
                          : factory.create_tcp(address, port, options.connect);
        auto connected = co_await stream->ensure_connected();
        if (connected) {
            co_return stream;
        }
        last = map_connect_error(connected.error(), tls);
        if (last.code == ErrorCode::CANCELLED) {
            co_return std::unexpected(std::move(last));
        }
    }
    co_return std::unexpected(std::move(last));
}

}  // namespace http
