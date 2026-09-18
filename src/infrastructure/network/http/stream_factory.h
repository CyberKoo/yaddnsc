//
// Stream factory abstraction for the net::http client domain.
//
// The Client drives this per URL scheme — https -> create_tls(),
// http -> create_tcp() — so the scheme/transport pairing lives in exactly
// one place and an injector can never cross-wire them.
//

#ifndef YADDNSC_HTTP_CLIENT_STREAM_FACTORY_H
#define YADDNSC_HTTP_CLIENT_STREAM_FACTORY_H

#include <cstdint>
#include <memory>
#include <string_view>

#include "infrastructure/network/transport/stream.h"
#include "support/util/cancellation_token.hpp"

namespace Transport {
struct Options;
struct TlsOptions;
}  // namespace Transport

namespace net::http {

/// Creates transport streams, split by intent (TLS vs plain TCP).
class StreamFactory {
public:
    virtual ~StreamFactory() = default;

    [[nodiscard]] virtual std::unique_ptr<Transport::Stream> create_tls(std::string_view host,
                                                                        std::uint16_t port,
                                                                        const Transport::Options& conn_opts,
                                                                        const Transport::TlsOptions& tls_opts) = 0;

    [[nodiscard]] virtual std::unique_ptr<Transport::Stream> create_tcp(std::string_view host,
                                                                        std::uint16_t port,
                                                                        const Transport::Options& opts) = 0;
};

/// Default factory: TlsStream for TLS, TcpStream for TCP.
///
/// Holds the cancellation token; every stream it creates is cancellable
/// through it.
class DefaultStreamFactory final : public StreamFactory {
public:
    explicit DefaultStreamFactory(Utils::CancellationToken token = {});

    [[nodiscard]] std::unique_ptr<Transport::Stream> create_tls(std::string_view host,
                                                                std::uint16_t port,
                                                                const Transport::Options& conn_opts,
                                                                const Transport::TlsOptions& tls_opts) override;

    [[nodiscard]] std::unique_ptr<Transport::Stream> create_tcp(std::string_view host,
                                                                std::uint16_t port,
                                                                const Transport::Options& conn_opts) override;

private:
    Utils::CancellationToken token_;
};

}  // namespace net::http

#endif  // YADDNSC_HTTP_CLIENT_STREAM_FACTORY_H
