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
#include "infrastructure/network/http/wire_request.h"
#include "infrastructure/network/transport/stream.h"

namespace net::http {

namespace {

/// Requests safe to replay automatically after a rebuilt connection:
/// methods with idempotent semantics (RFC 9110 §9.2.2).
[[nodiscard]] bool is_idempotent(const Method m) noexcept {
    using enum Method;
    return m == GET || m == HEAD || m == OPTIONS || m == PUT || m == DEL;
}

}  // namespace

Session::Session(SessionEnvironment env, SessionOrigin origin, const Limits limits)
    : env_(std::move(env)), origin_(std::move(origin)), limits_(limits) {}

std::expected<Response, Error> Session::exchange(const protocol::WireRequest& req,
                                                 const Utils::CancellationToken& token) {
    std::lock_guard lock(mutex_);

    if (stream_ && keep_alive_deadline_ && std::chrono::steady_clock::now() >= *keep_alive_deadline_) {
        stream_->close();
        stream_.reset();
        pending_.clear();
        keep_alive_remaining_.reset();
        keep_alive_deadline_.reset();
    }
    if (auto ready = ensure_stream(token); !ready) {
        return std::unexpected(std::move(ready.error()));
    }

    auto raw = do_exchange(req, token);
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
        if (auto ready = ensure_stream(token); !ready) {
            return std::unexpected(std::move(ready.error()));
        }
        raw = do_exchange(req, token);
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

std::expected<protocol::RawResponse, Error> Session::do_exchange(const protocol::WireRequest& req,
                                                                 const Utils::CancellationToken& token) {
    return protocol::exchange(*stream_, req, limits_, pending_, token);
}

std::expected<void, Error> Session::ensure_stream(const Utils::CancellationToken& token) {
    if (!stream_) {
        // Scheme/transport pairing is decided here, once per origin.
        stream_ = origin_.scheme == "https"
                      ? env_.factory->create_tls(origin_.host, origin_.port, env_.transport_opts, env_.tls_opts)
                      : env_.factory->create_tcp(origin_.host, origin_.port, env_.transport_opts);
    }
    if (auto connected = stream_->ensure_connected(token); !connected) {
        stream_.reset();
        pending_.clear();
        keep_alive_remaining_.reset();
        keep_alive_deadline_.reset();
        return std::unexpected(map_connect_error(connected.error()));
    }
    return {};
}

}  // namespace net::http
