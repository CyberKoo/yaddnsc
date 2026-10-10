//
// http — persistent client: fixed origin, reused connection.
//
// Same-origin exchanges reuse one connection (HTTP/1.1 persists by default;
// HTTP/1.0 only after explicit `Connection: keep-alive`). A redirect that leaves
// the origin is followed with a one-shot transient exchange, because the
// persistent connection belongs to its own origin.
//

#ifndef YADDNSC_INFRASTRUCTURE_HTTP_PERSISTENT_CLIENT_H
#define YADDNSC_INFRASTRUCTURE_HTTP_PERSISTENT_CLIENT_H

#include <cstdint>
#include <string>

#include <expected>

#include "coro/task.hpp"
#include "infrastructure/http/error.h"
#include "infrastructure/http/session.h"
#include "infrastructure/http/types.h"

namespace http {

/// The origin a persistent client is bound to.
struct Origin {
    std::string scheme;
    std::string host;
    std::uint16_t port{0};
};

/// HTTP client bound to one origin.
///
/// Ownership: owns its session (and therefore its connection). The returned task
/// borrows `this`.
/// Failure: expected<Response, Error>; the constructor rejects an invalid base
/// URL by throwing std::invalid_argument (a precondition the caller cannot
/// recover from).
/// Thread safety: the session serializes concurrent exchanges.
class PersistentClient {
public:
    /// @param base_url  Origin, e.g. "https://api.example.com" (a path is
    ///                  allowed and only used for relative fallback).
    /// @param options   Client options, including the injected hostname resolver.
    /// @throws std::invalid_argument when base_url is not a valid http(s) URL.
    explicit PersistentClient(std::string base_url, Options options = {});

    PersistentClient(const PersistentClient&) = delete;
    PersistentClient& operator=(const PersistentClient&) = delete;
    PersistentClient(PersistentClient&&) = delete;
    PersistentClient& operator=(PersistentClient&&) = delete;
    ~PersistentClient();

    /// Exchange against the session's origin.
    ///
    /// `target` is a path plus query ("/v1/update?foo=bar"). An absolute URL is
    /// also accepted; only its path+query is used, because the origin always
    /// comes from the base URL.
    [[nodiscard]] coro::Task<std::expected<Response, Error>> exchange(std::string target, const Request& request);

    /// Drop the connection; the next exchange reconnects.
    void close() noexcept;

    [[nodiscard]] const std::string& scheme() const noexcept { return scheme_; }

    [[nodiscard]] const std::string& host() const noexcept { return host_; }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    /// Delegated to by the public constructor once the origin is parsed, so the
    /// non-movable Session is built in place with its final origin.
    PersistentClient(Origin origin, Options options);

    std::string scheme_;
    std::string host_;
    std::uint16_t port_{0};
    Options options_;
    Session session_;
};

}  // namespace http

#endif  // YADDNSC_INFRASTRUCTURE_HTTP_PERSISTENT_CLIENT_H
