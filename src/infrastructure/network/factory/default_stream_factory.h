#ifndef YADDNSC_INFRASTRUCTURE_NETWORK_FACTORY_DEFAULT_STREAM_FACTORY_H
#define YADDNSC_INFRASTRUCTURE_NETWORK_FACTORY_DEFAULT_STREAM_FACTORY_H

#include <cstdint>
#include <memory>

#include "domain/network/inet_address.h"
#include "infrastructure/network/transport/options.h"
#include "infrastructure/network/transport/stream.h"

namespace net {

/// Assembles TLS and TCP implementations above the transport interface.
/// Returns exclusively owned streams; construction may throw std::bad_alloc.
/// Loop-thread only, like StreamFactory.
class DefaultStreamFactory final : public StreamFactory {
public:
    [[nodiscard]] std::unique_ptr<Stream> create_tls(domain::InetAddress address, std::uint16_t port,
                                                     const ConnectOptions& options, const TlsOptions& tls_options,
                                                     std::shared_ptr<const TlsContext> tls_context) override;

    [[nodiscard]] std::unique_ptr<Stream> create_tcp(domain::InetAddress address, std::uint16_t port,
                                                     const ConnectOptions& options) override;
};

}  // namespace net

#endif  // YADDNSC_INFRASTRUCTURE_NETWORK_FACTORY_DEFAULT_STREAM_FACTORY_H
