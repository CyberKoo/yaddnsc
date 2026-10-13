//
// Unit tests for the persistent side of the HTTP client
// (src/infrastructure/http/session.cpp and persistent_client.cpp).
//
// Covers: the lost-connection replay policy (which methods are idempotent, when
// a request must be explicitly replay-safe), the keep-alive request budget, the
// session lock's queue timeout, the connect/resolve failure paths of
// ensure_stream, and the persistent client's origin parsing, target resolution
// and redirect handling (same origin on the reused connection, a cross-origin
// hop through a transient client).
//
// Everything runs on a scripted in-memory stream (the net::Stream injection
// point) or on a ManualClock; there is no socket, no DNS and no filesystem I/O.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; the coroutines here use EXPECT_* only.
//

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <expected>
#include <gtest/gtest.h>

#include "coro/coro.h"
#include "infrastructure/http/persistent_client.h"
#include "infrastructure/http/session.h"
#include "infrastructure/http/types.h"
#include "infrastructure/network/transport/stream.h"

namespace {

using namespace std::chrono_literals;

using http::Error;
using http::ErrorCode;
using http::Options;
using http::Request;

const Request NO_REQUEST;

/// A bodyless 200 with a keep-alive request budget of `max` (0 = unbounded).
[[nodiscard]] std::string ok_with_max(const unsigned max) {
    return "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nKeep-Alive: max=" + std::to_string(max) + "\r\n\r\n";
}

const std::string OK = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
const std::string EMPTY = "";  ///< an immediate EOF: the peer closed

/// What one scripted connection does.
struct Behaviour {
    std::string incoming{};                  ///< bytes the peer serves; empty means an immediate EOF
    std::vector<bool> connect_results{};     ///< ensure_connected() results, consumed in order
    bool connect_afterwards{true};           ///< once `connect_results` is exhausted
    std::chrono::milliseconds read_stall{};  ///< park once before the first read
};

/// Everything one scripted connection saw, plus the health flag a test can flip.
struct ConnectionRecord {
    std::string sent;
    std::atomic_bool healthy{true};
};

/// A scripted in-memory stream: serves `behaviour` and records what was written.
class ScriptedStream : public net::Stream {
public:
    ScriptedStream(Behaviour behaviour, std::shared_ptr<ConnectionRecord> record)
        : behaviour_(std::move(behaviour)), record_(std::move(record)) {}

    [[nodiscard]] coro::Task<std::expected<void, net::IoError>> ensure_connected() override {
        const std::size_t index = connect_calls_++;
        const bool ok = index < behaviour_.connect_results.size() ? behaviour_.connect_results[index]
                                                                  : behaviour_.connect_afterwards;
        co_return ok ? std::expected<void, net::IoError>{} : std::unexpected(net::IoError::CONNECTION_FAILED);
    }

    [[nodiscard]] coro::Task<std::expected<std::size_t, net::IoError>> read_some(
        const std::span<std::uint8_t> buf) override {
        if (behaviour_.read_stall.count() > 0 && !stalled_) {
            stalled_ = true;
            co_await coro::sleep_for(behaviour_.read_stall);
        }
        if (cursor_ >= behaviour_.incoming.size()) {
            co_return std::size_t{0};  // EOF
        }
        const std::size_t available = behaviour_.incoming.size() - cursor_;
        const std::size_t count = std::min({buf.size(), available});
        std::memcpy(buf.data(), behaviour_.incoming.data() + cursor_, count);
        cursor_ += count;
        co_return count;
    }

    [[nodiscard]] coro::Task<std::expected<void, net::IoError>> read_exact(const std::span<std::uint8_t> buf) override {
        std::size_t filled = 0;
        while (filled < buf.size()) {
            auto read = co_await read_some(buf.subspan(filled));
            if (!read) {
                co_return std::unexpected(read.error());
            }
            if (*read == 0) {
                co_return std::unexpected(net::IoError::CONNECTION_FAILED);
            }
            filled += *read;
        }
        co_return {};
    }

    [[nodiscard]] coro::Task<std::expected<void, net::IoError>> send_all(
        const std::span<const std::uint8_t> data) override {
        record_->sent.append(reinterpret_cast<const char*>(data.data()), data.size());
        co_return {};
    }

    void close() noexcept override {}

    [[nodiscard]] bool connected() const noexcept override { return record_->healthy.load(); }

private:
    Behaviour behaviour_;
    std::shared_ptr<ConnectionRecord> record_;
    std::size_t connect_calls_{0};
    std::size_t cursor_{0};
    bool stalled_{false};
};

/// Hands out one scripted connection per call, so every reconnect gets a fresh peer.
class ScriptedFactory final : public net::StreamFactory {
public:
    explicit ScriptedFactory(std::deque<Behaviour> behaviours = {}) : behaviours_(std::move(behaviours)) {}

    [[nodiscard]] std::unique_ptr<net::Stream> create_tls(domain::InetAddress, std::uint16_t,
                                                          const net::ConnectOptions&, const net::TlsOptions&,
                                                          std::shared_ptr<const net::TlsContext>) override {
        ++tls_calls;
        return open();
    }

    [[nodiscard]] std::unique_ptr<net::Stream> create_tcp(domain::InetAddress, std::uint16_t,
                                                          const net::ConnectOptions&) override {
        ++tcp_calls;
        return open();
    }

    /// The connections handed out so far, in order.
    [[nodiscard]] const std::vector<std::shared_ptr<ConnectionRecord>>& connections() const noexcept {
        return records_;
    }

    [[nodiscard]] const std::string& sent_on(const std::size_t index) const { return records_.at(index)->sent; }

    /// Mark the peer of connection `index` as gone.
    void drop(const std::size_t index) const { records_.at(index)->healthy.store(false); }

    /// Queue one more scripted connection, for a case that reconnects later.
    void queue(Behaviour behaviour) { behaviours_.push_back(std::move(behaviour)); }

    int tcp_calls{0};
    int tls_calls{0};

private:
    [[nodiscard]] std::unique_ptr<net::Stream> open() {
        Behaviour behaviour;
        if (!behaviours_.empty()) {
            behaviour = std::move(behaviours_.front());
            behaviours_.pop_front();
        }
        records_.push_back(std::make_shared<ConnectionRecord>());
        return std::make_unique<ScriptedStream>(std::move(behaviour), records_.back());
    }

    std::deque<Behaviour> behaviours_;
    std::vector<std::shared_ptr<ConnectionRecord>> records_;
};

[[nodiscard]] std::shared_ptr<ScriptedFactory> factory_for(std::deque<Behaviour> behaviours) {
    return std::make_shared<ScriptedFactory>(std::move(behaviours));
}

[[nodiscard]] Options options_for(const std::shared_ptr<ScriptedFactory>& factory) {
    Options options;
    options.factory = factory;
    options.connect_timeout = 0ms;  // no per-address budget unless a case asks for one
    return options;
}

[[nodiscard]] http::Session session_for(const Options& options, const std::string_view host = "203.0.113.10") {
    return http::Session{options, "http", std::string(host), 8080};
}

// ---------------------------------------------------------------------------
// Session: the lost-connection replay policy
// ---------------------------------------------------------------------------

TEST(HttpSessionReplay, idempotentMethods_AreReplayedOnARebuiltConnection) {
    // RFC 9110 §9.2.2: GET/HEAD/OPTIONS/PUT/DELETE may be replayed after a
    // connection is lost, so a rebuilt connection retries the request once.
    for (const auto method :
         {http::Method::GET, http::Method::HEAD, http::Method::OPTIONS, http::Method::PUT, http::Method::DEL}) {
        SCOPED_TRACE(static_cast<int>(method));
        auto factory = factory_for({Behaviour{.incoming = EMPTY}, Behaviour{.incoming = OK}});
        const Options options = options_for(factory);
        http::Session session = session_for(options);
        Request request;
        request.method = method;

        const auto response = coro::run(session.exchange("/x", request));

        ASSERT_TRUE(response.has_value());
        EXPECT_EQ(response->status_, 200);
        EXPECT_EQ(factory->tcp_calls, 2);
    }
}

TEST(HttpSessionReplay, nonIdempotentMethods_AreNotReplayedByDefault) {
    // A POST that may have reached the server is never repeated unless the
    // caller declared the request replay-safe.
    for (const auto method : {http::Method::POST, http::Method::PATCH}) {
        SCOPED_TRACE(static_cast<int>(method));
        auto factory = factory_for({Behaviour{.incoming = EMPTY}});
        const Options options = options_for(factory);
        http::Session session = session_for(options);
        Request request;
        request.method = method;

        const auto response = coro::run(session.exchange("/x", request));

        ASSERT_FALSE(response.has_value());
        EXPECT_EQ(response.error().code, ErrorCode::CONNECTION_LOST);
        EXPECT_EQ(factory->tcp_calls, 1);
    }
}

TEST(HttpSessionReplay, replaySafeRequest_OnAReusedConnection_IsReplayed) {
    // Connection 1 answers the first exchange and is then at EOF, so the second
    // exchange fails on a *reused* connection — the case the replay-safe flag
    // exists for.
    auto factory = factory_for({Behaviour{.incoming = OK}, Behaviour{.incoming = OK}});
    const Options options = options_for(factory);
    http::Session session = session_for(options);
    Request request;
    request.method = http::Method::POST;
    request.replay_safe = true;

    ASSERT_TRUE(coro::run(session.exchange("/first", request)).has_value());
    const auto second = coro::run(session.exchange("/second", request));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->status_, 200);
    EXPECT_EQ(factory->tcp_calls, 2);
}

TEST(HttpSessionReplay, lostTwice_ReportsTheError) {
    auto factory = factory_for({Behaviour{.incoming = EMPTY}, Behaviour{.incoming = EMPTY}});
    const Options options = options_for(factory);
    http::Session session = session_for(options);

    const auto response = coro::run(session.exchange("/x", NO_REQUEST));

    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().code, ErrorCode::CONNECTION_LOST);
    EXPECT_EQ(factory->tcp_calls, 2);
}

// ---------------------------------------------------------------------------
// Session: the keep-alive request budget
// ---------------------------------------------------------------------------

TEST(HttpSessionKeepAlive, budgetOfZero_RetiresTheConnectionAfterOneExchange) {
    // `max=0` spends the whole budget on the first exchange, so the response is
    // delivered but the connection is not reused.
    auto factory = factory_for({Behaviour{.incoming = ok_with_max(0)}, Behaviour{.incoming = ok_with_max(0)}});
    const Options options = options_for(factory);
    http::Session session = session_for(options);

    EXPECT_TRUE(coro::run(session.exchange("/first", NO_REQUEST)).has_value());
    EXPECT_TRUE(coro::run(session.exchange("/second", NO_REQUEST)).has_value());
    EXPECT_EQ(factory->tcp_calls, 2);
}

TEST(HttpSessionKeepAlive, repeatedBudget_IsClampedToTheLowerValue) {
    // A second `max` must never raise a budget earlier exchanges already spent.
    auto factory = factory_for({Behaviour{.incoming = ok_with_max(5) + ok_with_max(0)}});
    const Options options = options_for(factory);
    http::Session session = session_for(options);

    EXPECT_TRUE(coro::run(session.exchange("/first", NO_REQUEST)).has_value());
    // max 5 -> 4 remaining after the first exchange; max 0 clamps it to 0, so
    // the second exchange is served on the same connection and then retires it.
    EXPECT_TRUE(coro::run(session.exchange("/second", NO_REQUEST)).has_value());
    EXPECT_EQ(factory->tcp_calls, 1);

    // The retired connection is replaced rather than reused.
    factory->queue(Behaviour{.incoming = OK});
    EXPECT_TRUE(coro::run(session.exchange("/third", NO_REQUEST)).has_value());
    EXPECT_EQ(factory->tcp_calls, 2);
}

// ---------------------------------------------------------------------------
// Session: the queue timeout on the session lock
// ---------------------------------------------------------------------------

TEST(HttpSessionQueue, secondExchange_ExpiresItsQueueBudget) {
    // The first exchange holds the session lock while parked on the wire; the
    // second one's queue budget runs out before the lock is handed over, which
    // is a local scheduling failure and must not touch the live connection.
    auto factory = factory_for({Behaviour{.incoming = OK, .read_stall = 50ms}});
    Options options = options_for(factory);
    options.queue_timeout = 5ms;
    http::Session session = session_for(options);

    coro::ManualClock clock;
    coro::Loop loop{clock};
    std::optional<http::Response> first;
    std::optional<Error> second;

    coro::run(loop, [&]() -> coro::Task<void> {
        co_await coro::task_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            auto holder = group.spawn(session.exchange("/first", NO_REQUEST));
            auto waiter = group.spawn(session.exchange("/second", NO_REQUEST));
            if (const auto held = co_await holder; held) {
                first.emplace(std::move(*held));
            }
            if (const auto waited = co_await waiter; !waited) {
                second = waited.error();
            }
        });
    }());

    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->status_, 200);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->code, ErrorCode::QUEUE_TIMEOUT);
    // The holder's connection was never taken away by the expired waiter.
    EXPECT_EQ(factory->tcp_calls, 1);
}

// ---------------------------------------------------------------------------
// Session: connect and resolve failures
// ---------------------------------------------------------------------------

TEST(HttpSessionConnect, unresolvableHost_IsReported) {
    auto factory = factory_for({});
    const Options options = options_for(factory);
    http::Session session = session_for(options, "example.test");

    const auto response = coro::run(session.exchange("/x", NO_REQUEST));

    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().code, ErrorCode::RESOLVE_FAILED);
    EXPECT_EQ(factory->tcp_calls, 0);
}

TEST(HttpSessionConnect, refusedConnection_IsReported) {
    auto factory = factory_for({Behaviour{.connect_results = {false}}});
    const Options options = options_for(factory);
    http::Session session = session_for(options);

    const auto response = coro::run(session.exchange("/x", NO_REQUEST));

    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().code, ErrorCode::CONNECT_FAILED);
    EXPECT_EQ(response.error().message, R"(Cannot connect to "203.0.113.10")");
}

TEST(HttpSessionConnect, deadConnection_IsDroppedAndReplaced) {
    auto factory = factory_for({Behaviour{.incoming = OK}, Behaviour{.incoming = OK}});
    const Options options = options_for(factory);
    http::Session session = session_for(options);

    ASSERT_TRUE(coro::run(session.exchange("/first", NO_REQUEST)).has_value());
    // The peer vanished after answering, so the cached address list must not be
    // reused: the session reconnects through the factory.
    factory->drop(0);

    const auto second = coro::run(session.exchange("/second", NO_REQUEST));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(factory->tcp_calls, 2);
}

TEST(HttpSessionConnect, unhealthyReusedStream_IsReportedAsAConnectFailure) {
    auto factory = factory_for({Behaviour{.incoming = OK, .connect_results = {true, false}}});
    const Options options = options_for(factory);
    http::Session session = session_for(options);

    ASSERT_TRUE(coro::run(session.exchange("/first", NO_REQUEST)).has_value());

    const auto second = coro::run(session.exchange("/second", NO_REQUEST));
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code, ErrorCode::CONNECT_FAILED);
    // The exchange never reached the wire, so nothing new was dialled.
    EXPECT_EQ(factory->tcp_calls, 1);
}

// ---------------------------------------------------------------------------
// PersistentClient: origin parsing
// ---------------------------------------------------------------------------

TEST(HttpPersistentClientOrigin, baseUrl_FillsInTheWellKnownPort) {
    auto factory = factory_for({Behaviour{.incoming = OK}});
    const Options options = options_for(factory);

    http::PersistentClient client{"https://api.example.test", options};

    EXPECT_EQ(client.scheme(), "https");
    EXPECT_EQ(client.host(), "api.example.test");
    EXPECT_EQ(client.port(), 443);
}

TEST(HttpPersistentClientOrigin, baseUrl_KeepsAnExplicitPort) {
    auto factory = factory_for({});
    const Options options = options_for(factory);

    http::PersistentClient client{"http://203.0.113.10:8080/base", options};

    EXPECT_EQ(client.scheme(), "http");
    EXPECT_EQ(client.host(), "203.0.113.10");
    EXPECT_EQ(client.port(), 8080);
}

TEST(HttpPersistentClientOrigin, baseUrl_KeepsABracketedIpv6Host) {
    auto factory = factory_for({Behaviour{.incoming = OK}});
    const Options options = options_for(factory);

    http::PersistentClient client{"http://[2001:db8::1]:8080", options};
    ASSERT_TRUE(coro::run(client.exchange("/x", NO_REQUEST)).has_value());

    // The bracketed literal has to survive the reassembled request URL and the
    // Host header, both of which must stay parseable by the next hop.
    EXPECT_EQ(client.host(), "2001:db8::1");
    EXPECT_NE(factory->sent_on(0).find("Host: [2001:db8::1]:8080"), std::string::npos);
}

TEST(HttpPersistentClientOrigin, invalidBaseUrl_IsRejectedAtConstruction) {
    auto factory = factory_for({});
    const Options options = options_for(factory);

    // An unparsable URL, a non-http(s) scheme and a URL without a host are all
    // unrecoverable at this layer, so construction throws instead of returning.
    for (const auto& base_url : {"http://[::1/", "ftp://203.0.113.10/", "", "http://:8080/"}) {
        SCOPED_TRACE(base_url);
        EXPECT_THROW((http::PersistentClient{std::string(base_url), options}), std::invalid_argument);
    }
}

// ---------------------------------------------------------------------------
// PersistentClient: target resolution
// ---------------------------------------------------------------------------

TEST(HttpPersistentClientTarget, emptyTarget_AsksForTheRootPath) {
    auto factory = factory_for({Behaviour{.incoming = OK}});
    const Options options = options_for(factory);
    http::PersistentClient client{"http://203.0.113.10:8080", options};

    ASSERT_TRUE(coro::run(client.exchange("", NO_REQUEST)).has_value());
    EXPECT_TRUE(factory->sent_on(0).starts_with("GET / HTTP/1.1\r\n"));
}

TEST(HttpPersistentClientTarget, barePath_IsUsedVerbatim) {
    auto factory = factory_for({Behaviour{.incoming = OK}});
    const Options options = options_for(factory);
    http::PersistentClient client{"http://203.0.113.10:8080", options};

    ASSERT_TRUE(coro::run(client.exchange("/dns-query?name=example.test", NO_REQUEST)).has_value());
    EXPECT_TRUE(factory->sent_on(0).starts_with("GET /dns-query?name=example.test HTTP/1.1\r\n"));
}

TEST(HttpPersistentClientTarget, absoluteUrl_ContributesOnlyItsPathAndQuery) {
    // The origin always comes from the base URL, so an absolute target must not
    // be able to move the request to another host.
    auto factory = factory_for({Behaviour{.incoming = OK}});
    const Options options = options_for(factory);
    http::PersistentClient client{"http://203.0.113.10:8080", options};

    ASSERT_TRUE(coro::run(client.exchange("https://other.test:9090/abs?y=2#frag", NO_REQUEST)).has_value());
    EXPECT_TRUE(factory->sent_on(0).starts_with("GET /abs?y=2 HTTP/1.1\r\n"));
    EXPECT_NE(factory->sent_on(0).find("Host: 203.0.113.10:8080"), std::string::npos);
    EXPECT_EQ(factory->sent_on(0).find("other.test"), std::string::npos);
}

TEST(HttpPersistentClientTarget, unparsableTarget_IsRejectedBeforeConnecting) {
    auto factory = factory_for({});
    const Options options = options_for(factory);
    http::PersistentClient client{"http://203.0.113.10:8080", options};

    const auto response = coro::run(client.exchange("http://[::1/", NO_REQUEST));

    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().code, ErrorCode::INVALID_URL);
    EXPECT_EQ(factory->tcp_calls, 0);
}

TEST(HttpPersistentClientTarget, invalidRequestHeader_IsRejectedBeforeConnecting) {
    auto factory = factory_for({});
    const Options options = options_for(factory);
    http::PersistentClient client{"http://203.0.113.10:8080", options};
    Request request;
    request.headers.emplace("Invalid Header", "value");

    const auto response = coro::run(client.exchange("/x", request));

    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().code, ErrorCode::INVALID_REQUEST);
    EXPECT_EQ(factory->tcp_calls, 0);
}

// ---------------------------------------------------------------------------
// PersistentClient: redirects
// ---------------------------------------------------------------------------

TEST(HttpPersistentClientRedirect, sameOrigin_StaysOnTheReusedConnection) {
    auto factory =
        factory_for({Behaviour{.incoming = "HTTP/1.1 302 Found\r\nLocation: /next\r\nContent-Length: 0\r\n\r\n" + OK}});
    const Options options = options_for(factory);
    http::PersistentClient client{"http://203.0.113.10:8080", options};

    const auto response = coro::run(client.exchange("/start", NO_REQUEST));

    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->status_, 200);
    // The whole chain is one exchange on the session's own connection.
    EXPECT_EQ(factory->tcp_calls, 1);
    const auto& sent = factory->sent_on(0);
    EXPECT_NE(sent.find("GET /start HTTP/1.1\r\n"), std::string::npos);
    EXPECT_NE(sent.find("GET /next HTTP/1.1\r\n"), std::string::npos);
}

TEST(HttpPersistentClientRedirect, crossOrigin_FallsBackToATransientHop) {
    auto factory =
        factory_for({Behaviour{.incoming = "HTTP/1.1 302 Found\r\nLocation: http://203.0.113.20:8080/other\r\n"
                                           "Content-Length: 0\r\n\r\n"},
                     Behaviour{.incoming = OK}});
    const Options options = options_for(factory);
    http::PersistentClient client{"http://203.0.113.10:8080", options};

    const auto response = coro::run(client.exchange("/start", NO_REQUEST));

    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->status_, 200);
    // The second origin cannot be served by this session's connection.
    EXPECT_EQ(factory->tcp_calls, 2);
    EXPECT_NE(factory->sent_on(1).find("Host: 203.0.113.20:8080"), std::string::npos);
}

TEST(HttpPersistentClientRedirect, beyondTheLimit_ReportsTheLimit) {
    auto factory =
        factory_for({Behaviour{.incoming = "HTTP/1.1 302 Found\r\nLocation: /one\r\nContent-Length: 0\r\n\r\n"
                                           "HTTP/1.1 302 Found\r\nLocation: /two\r\nContent-Length: 0\r\n\r\n"}});
    Options options = options_for(factory);
    options.max_redirects = 1;
    http::PersistentClient client{"http://203.0.113.10:8080", options};

    const auto response = coro::run(client.exchange("/start", NO_REQUEST));

    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().code, ErrorCode::REDIRECT_LIMIT_EXCEEDED);
    EXPECT_EQ(factory->tcp_calls, 1);
}

TEST(HttpPersistentClientRedirect, disabledFollowing_LeavesTheRedirectResponse) {
    auto factory =
        factory_for({Behaviour{.incoming = "HTTP/1.1 302 Found\r\nLocation: /one\r\nContent-Length: 4\r\n\r\nbody"}});
    Options options = options_for(factory);
    options.follow_redirects = false;
    http::PersistentClient client{"http://203.0.113.10:8080", options};

    const auto response = coro::run(client.exchange("/start", NO_REQUEST));

    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->status_, 302);
    EXPECT_EQ(response->text(), "body");
    EXPECT_EQ(factory->tcp_calls, 1);
}

// ---------------------------------------------------------------------------
// PersistentClient: failures and close
// ---------------------------------------------------------------------------

TEST(HttpPersistentClientFailure, exchangeFailure_IsPropagatedUnchanged) {
    // A POST is not replayed, so the lost connection surfaces as-is.
    auto factory = factory_for({Behaviour{.incoming = EMPTY}});
    const Options options = options_for(factory);
    http::PersistentClient client{"http://203.0.113.10:8080", options};
    Request request;
    request.method = http::Method::POST;

    const auto response = coro::run(client.exchange("/x", request));

    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().code, ErrorCode::CONNECTION_LOST);
    EXPECT_EQ(factory->tcp_calls, 1);
}

TEST(HttpPersistentClientFailure, close_DropsTheConnectionForTheNextExchange) {
    auto factory = factory_for({Behaviour{.incoming = OK}, Behaviour{.incoming = OK}});
    const Options options = options_for(factory);
    http::PersistentClient client{"http://203.0.113.10:8080", options};

    ASSERT_TRUE(coro::run(client.exchange("/first", NO_REQUEST)).has_value());
    EXPECT_EQ(factory->tcp_calls, 1);

    client.close();
    EXPECT_TRUE(coro::run(client.exchange("/second", NO_REQUEST)).has_value());
    EXPECT_EQ(factory->tcp_calls, 2);
}

}  // namespace
