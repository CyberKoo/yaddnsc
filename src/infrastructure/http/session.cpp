//
// http — persistent session over one reused connection.
//

#include "session.h"

#include <algorithm>
#include <chrono>
#include <compare>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include <coroutine>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include <expected>
#include <map>
#include <utility>
#include <span>
#include <vector>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not

#include "domain/network/inet_address.h"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include "infrastructure/http/protocol/wire.h"
#include "infrastructure/http/transport.h"
#include "infrastructure/http/wire_request.h"
#include "infrastructure/coro/cancelled.h"
#include "infrastructure/network/transport/stream.h"

namespace http {
namespace {

/// Methods safe to replay after a rebuilt connection (RFC 9110 §9.2.2).
[[nodiscard]] bool is_idempotent(const Method method) noexcept {
    return method == Method::GET || method == Method::HEAD || method == Method::OPTIONS || method == Method::PUT ||
           method == Method::DEL;
}

}  // namespace

Session::Session(Options options, std::string scheme, std::string host, const std::uint16_t port)
    : options_(std::move(options)), scheme_(std::move(scheme)), host_(std::move(host)), port_(port) {}

Session::~Session() {
    close();
}

coro::Task<std::expected<Response, Error>> Session::exchange(std::string target, const Request& request) {
    // Waiting semantics: a second concurrent exchange queues here instead of
    // being rejected. A cancelled wait leaves the session untouched.
    auto guard = co_await mutex_.lock();
    try {
        // A close() that arrived while an exchange held the guard is honoured now,
        // under the guard, before the connection is reused.
        if (close_requested_) {
            close_requested_ = false;
            drop_connection();
        }

        if (auto valid = validate_request(request); !valid) {
            co_return std::unexpected(std::move(valid.error()));
        }

        // An expired keep-alive deadline retires the connection before reuse.
        if (stream_ != nullptr && keep_alive_deadline_.has_value() &&
            std::chrono::steady_clock::now() >= *keep_alive_deadline_) {
            drop_connection();
        }
        if (auto ready = co_await ensure_stream(); !ready) {
            co_return std::unexpected(std::move(ready.error()));
        }

        auto wire = build_wire_request(request, scheme_, host_, port_, options_);
        wire.target = std::move(target);

        auto raw = co_await do_exchange(wire);
        if (!raw) {
            const bool retry = raw.error().code == ErrorCode::CONNECTION_LOST && is_idempotent(wire.method);
            // A failed exchange can leave unread bytes on the stream, so it is never
            // reused: either the retry rebuilds it or the failure is reported.
            drop_connection();
            if (!retry) {
                co_return std::unexpected(std::move(raw.error()));
            }
            if (auto ready = co_await ensure_stream(); !ready) {
                co_return std::unexpected(std::move(ready.error()));
            }
            raw = co_await do_exchange(wire);
            if (!raw) {
                drop_connection();
                co_return std::unexpected(std::move(raw.error()));
            }
        }

        if (raw->keep_alive_max.has_value()) {
            // `max` caps the requests on this connection; a repeated header must not
            // reset a budget earlier exchanges already consumed.
            keep_alive_remaining_ = keep_alive_remaining_.has_value()
                                        ? std::min(*keep_alive_remaining_, *raw->keep_alive_max)
                                        : *raw->keep_alive_max;
        }
        if (raw->keep_alive_timeout.has_value()) {
            keep_alive_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(*raw->keep_alive_timeout);
        }
        if (keep_alive_remaining_.has_value()) {
            if (*keep_alive_remaining_ > 0) {
                --*keep_alive_remaining_;
            }
            if (*keep_alive_remaining_ == 0) {
                raw->reusable = false;
            }
        }

        if (!raw->reusable) {
            drop_connection();
        }
        // A close() requested mid-exchange drops the connection as soon as the
        // exchange completes, still under the guard.
        if (close_requested_) {
            close_requested_ = false;
            drop_connection();
        }
        co_return Response{raw->status, std::move(raw->body), std::move(raw->headers), std::move(raw->trailers)};
    } catch (const coro::Cancelled&) {
        drop_connection();
        throw;
    }
}

coro::Task<std::expected<protocol::RawResponse, Error>> Session::do_exchange(const protocol::WireRequest& wire) {
    co_return co_await protocol::exchange(*stream_, wire, options_.limits, pending_);
}

coro::Task<std::expected<void, Error>> Session::ensure_stream() {
    if (stream_ != nullptr && !stream_->connected()) {
        // A dead connection is dropped so the reconnect re-resolves the
        // hostname instead of redialling the cached address list.
        drop_connection();
    }
    if (stream_ == nullptr) {
        auto addresses = co_await resolve_host(host_, options_);
        if (!addresses) {
            co_return std::unexpected(std::move(addresses.error()));
        }
        auto stream = co_await connect_stream(scheme_, host_, *addresses, port_, options_);
        if (!stream) {
            co_return std::unexpected(std::move(stream.error()));
        }
        stream_ = std::move(*stream);
        co_return {};
    }
    if (auto connected = co_await stream_->ensure_connected(); !connected) {
        drop_connection();
        co_return std::unexpected(connect_error());
    }
    co_return {};
}

void Session::close() noexcept {
    // Never touch stream_ outside the session guard: an in-flight exchange
    // borrows it by reference for the whole exchange. While the mutex is held
    // the close is deferred to the guard's next checkpoint inside exchange();
    // an idle session drops the connection immediately.
    if (mutex_.locked()) {
        close_requested_ = true;
        return;
    }
    drop_connection();
}

void Session::drop_connection() noexcept {
    if (stream_ != nullptr) {
        stream_->close();
        stream_.reset();
    }
    pending_.clear();
    keep_alive_remaining_.reset();
    keep_alive_deadline_.reset();
}

}  // namespace http
