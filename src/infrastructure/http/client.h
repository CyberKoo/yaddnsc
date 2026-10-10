//
// http — transient client: one connection per hop, full redirect loop.
//
// The caller sees one `exchange(url, request)` operation; connection lifecycle
// and TLS are entirely internal. Use Session or PersistentClient when the caller
// wants connection reuse.
//

#ifndef YADDNSC_INFRASTRUCTURE_HTTP_CLIENT_H
#define YADDNSC_INFRASTRUCTURE_HTTP_CLIENT_H

#include <string>

#include <expected>

#include "infrastructure/coro/task.hpp"
#include "infrastructure/http/error.h"
#include "infrastructure/http/types.h"

namespace http {

/// One-URL-per-request HTTP client.
///
/// Ownership: borrows nothing; the returned task borrows `this`, so the client
/// must outlive the exchange.
/// Failure: expected<Response, Error>. The URL must be absolute; following
/// redirects is decided by Options::follow_redirects.
/// Cancellation: the awaiting task's cancel scope; a cancelled await surfaces as
/// `coro::Cancelled`, and a caller-imposed deadline is reported by that scope
/// (`ScopeOutcome::timed_out()`), never by this layer.
/// Thread safety: an exchange only mutates the connection it created, so
/// distinct concurrent exchanges on one client are safe; each owns its stream.
class Client {
public:
    explicit Client(Options options = {});

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) = delete;
    Client& operator=(Client&&) = delete;
    ~Client() = default;

    /// Perform an exchange against an absolute `http://` or `https://` URL.
    [[nodiscard]] coro::Task<std::expected<Response, Error>> exchange(std::string url, const Request& request);

private:
    Options options_;
};

}  // namespace http

#endif  // YADDNSC_INFRASTRUCTURE_HTTP_CLIENT_H
