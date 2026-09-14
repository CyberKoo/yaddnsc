//
// Session — persistent, thread-safe HTTP exchange over one reused
// connection (net::http).
//
#include "http_client/session.h"

#include <utility>

#include "network/transport/stream.h"

#include "fmt.hpp"

namespace net::http {

namespace {

[[nodiscard]] Error map_connect_error(const Transport::IoError err) {
    using enum Transport::IoError;
    switch (err) {
        case CANCELLED:
            return {ErrorCode::CANCELLED, "connect/handshake: cancelled"};
        case TIMEOUT:
            return {ErrorCode::TIMEOUT, "connect/handshake: timed out"};
        case CONNECTION_FAILED:
            return {ErrorCode::CONNECT_FAILED, "connect/handshake failed"};
    }
    return {ErrorCode::CONNECT_FAILED, "connect/handshake failed"};
}

/// Requests safe to replay automatically after a rebuilt connection:
/// methods with idempotent semantics (RFC 9110 §9.2.2).
[[nodiscard]] bool is_idempotent(const Method m) noexcept {
    using enum Method;
    return m == GET || m == HEAD || m == OPTIONS || m == PUT || m == DEL;
}

}  // namespace

Session::Session(std::shared_ptr<StreamFactory> factory,
                 Transport::Options transport_opts,
                 Transport::TlsOptions tls_opts,
                 std::string scheme,
                 std::string host,
                 const std::uint16_t port,
                 const Limits limits)
    : factory_(std::move(factory)), transport_opts_(std::move(transport_opts)), tls_opts_(std::move(tls_opts)),
      scheme_(std::move(scheme)), host_(std::move(host)), port_(port), limits_(limits) {}

std::expected<Response, Error> Session::exchange(const protocol::WireRequest& req) {
    std::lock_guard lock(mutex_);

    if (auto ready = ensure_stream(); !ready) {
        return std::unexpected(std::move(ready.error()));
    }

    auto raw = do_exchange(req);
    if (!raw) {
        if (raw.error().code == ErrorCode::CONNECTION_LOST && is_idempotent(req.method)) {
            // Rebuild the connection and retry once.
            stream_.reset();
            if (auto ready = ensure_stream(); !ready) {
                return std::unexpected(std::move(ready.error()));
            }
            raw = do_exchange(req);
        }
        if (!raw) {
            return std::unexpected(std::move(raw.error()));
        }
    }

    return Response{raw->status, std::move(raw->body), std::move(raw->headers)};
}

std::expected<protocol::RawResponse, Error> Session::do_exchange(const protocol::WireRequest& req) {
    return protocol::exchange(*stream_, req, limits_);
}

std::expected<void, Error> Session::ensure_stream() {
    if (!stream_) {
        // Scheme/transport pairing is decided here, once per origin.
        stream_ = scheme_ == "https"
                      ? factory_->create_tls(host_, port_, transport_opts_, tls_opts_)
                      : factory_->create_tcp(host_, port_, transport_opts_);
    }
    if (auto connected = stream_->ensure_connected(); !connected) {
        stream_.reset();
        return std::unexpected(map_connect_error(connected.error()));
    }
    return {};
}

}  // namespace net::http
