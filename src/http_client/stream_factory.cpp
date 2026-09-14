//
// Stream factory abstraction for the net::http client domain.
//
#include "http_client/stream_factory.h"

#include <string>
#include <utility>

#include "network/transport/tcp_stream.h"
#include "network/transport/tls_stream.h"
#include "util/cancellation_token.hpp"

namespace net::http {

DefaultStreamFactory::DefaultStreamFactory(Utils::CancellationToken token) : token_(std::move(token)) {
}

std::unique_ptr<Transport::Stream>
DefaultStreamFactory::create_tls(const std::string_view host, const std::uint16_t port,
                                 const Transport::Options &conn_opts, const Transport::TlsOptions &tls_opts) {
    return std::make_unique<Transport::TlsStream>(std::string(host), port, conn_opts, tls_opts, token_);
}

std::unique_ptr<Transport::Stream>
DefaultStreamFactory::create_tcp(const std::string_view host, const std::uint16_t port,
                                 const Transport::Options &conn_opts) {
    return std::make_unique<Transport::TcpStream>(std::string(host), port, conn_opts, token_);
}

} // namespace net::http
