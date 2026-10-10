//
// net — the production stream factory.
//

#include "default_stream_factory.h"

#include <utility>
#include <memory>

#include "infrastructure/network/tls/stream.h"
#include "infrastructure/network/transport/tcp_stream.h"
#include "domain/network/inet_address.h"

namespace net {

std::unique_ptr<Stream> DefaultStreamFactory::create_tls(domain::InetAddress address, const std::uint16_t port,
                                                         const ConnectOptions& options, const TlsOptions& tls_options,
                                                         std::shared_ptr<const TlsContext> tls_context) {
    return std::make_unique<TlsStream>(address, port, std::move(tls_context), options, tls_options);
}

std::unique_ptr<Stream> DefaultStreamFactory::create_tcp(domain::InetAddress address, const std::uint16_t port,
                                                         const ConnectOptions& options) {
    return std::make_unique<TcpStream>(address, port, options);
}

}  // namespace net
