//
// Session — persistent, thread-safe HTTP exchange over one reused
// connection (net::http).
//
#include "infrastructure/network/http/session.h"

#include <algorithm>
#include <chrono>
#include <compare>
#include <map>
#include <utility>

#include "infrastructure/network/http/protocol/wire.h"
#include "infrastructure/network/http/stream_factory.h"
#include "infrastructure/network/transport/io_error.h"
#include "infrastructure/network/transport/stream.h"

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

    if (stream_ && keep_alive_deadline_ && std::chrono::steady_clock::now() >= *keep_alive_deadline_) {
        stream_->close();
        stream_.reset();
        pending_.clear();
        keep_alive_remaining_.reset();
        keep_alive_deadline_.reset();
    }
    if (auto ready = ensure_stream(); !ready) {
        return std::unexpected(std::move(ready.error()));
    }

    auto raw = do_exchange(req);
    if (!raw) {
        const auto should_retry = raw.error().code == ErrorCode::CONNECTION_LOST && is_idempotent(req.method);
        // A failed exchange can leave unread bytes or a partially consumed
        // response on the stream. Never issue another request on it.
        stream_->close();
        stream_.reset();
        pending_.clear();
        keep_alive_remaining_.reset();
        keep_alive_deadline_.reset();

        if (!should_retry) {
            return std::unexpected(std::move(raw.error()));
        }
        if (auto ready = ensure_stream(); !ready) {
            return std::unexpected(std::move(ready.error()));
        }
        raw = do_exchange(req);
        if (!raw) {
            stream_->close();
            stream_.reset();
            pending_.clear();
            keep_alive_remaining_.reset();
            keep_alive_deadline_.reset();
            return std::unexpected(std::move(raw.error()));
        }
    }

    if (raw->keep_alive_max) {
        // `max` limits requests on this connection; a repeated header must
        // not reset a cap already consumed by earlier exchanges.
        keep_alive_remaining_ =
            keep_alive_remaining_ ? std::min(*keep_alive_remaining_, *raw->keep_alive_max) : *raw->keep_alive_max;
    }
    if (raw->keep_alive_timeout) {
        keep_alive_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(*raw->keep_alive_timeout);
    }
    if (keep_alive_remaining_) {
        if (*keep_alive_remaining_ > 0) {
            --*keep_alive_remaining_;
        }
        if (*keep_alive_remaining_ == 0) {
            raw->reusable = false;
        }
    }

    if (!raw->reusable) {
        stream_->close();
        stream_.reset();
        pending_.clear();
        keep_alive_remaining_.reset();
        keep_alive_deadline_.reset();
    }
    return Response{raw->status, std::move(raw->body), std::move(raw->headers), std::move(raw->trailers)};
}

std::expected<protocol::RawResponse, Error> Session::do_exchange(const protocol::WireRequest& req) {
    return protocol::exchange(*stream_, req, limits_, pending_);
}

std::expected<void, Error> Session::ensure_stream() {
    if (!stream_) {
        // Scheme/transport pairing is decided here, once per origin.
        stream_ = scheme_ == "https" ? factory_->create_tls(host_, port_, transport_opts_, tls_opts_)
                                     : factory_->create_tcp(host_, port_, transport_opts_);
    }
    if (auto connected = stream_->ensure_connected(); !connected) {
        stream_.reset();
        pending_.clear();
        keep_alive_remaining_.reset();
        keep_alive_deadline_.reset();
        return std::unexpected(map_connect_error(connected.error()));
    }
    return {};
}

}  // namespace net::http
