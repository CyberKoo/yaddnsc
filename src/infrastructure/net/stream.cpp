//
// net — the production stream factory.
//

#include "stream.h"

#include <utility>

#include "infrastructure/net/tcp_stream.h"
#include "infrastructure/net/tls_stream.h"

namespace net {

std::unique_ptr<Stream> DefaultStreamFactory::create_tls(InetAddress address, const std::uint16_t port,
                                                         const ConnectOptions& options, const TlsOptions& tls_options) {
    return std::make_unique<TlsStream>(std::move(address), port, options, tls_options);
}

std::unique_ptr<Stream> DefaultStreamFactory::create_tcp(InetAddress address, const std::uint16_t port,
                                                         const ConnectOptions& options) {
    return std::make_unique<TcpStream>(std::move(address), port, options);
}

}  // namespace net
